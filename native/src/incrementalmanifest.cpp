#include "incrementalmanifest.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <cmath>

namespace {

bool fields(const QJsonObject &object, const QStringList &names)
{
    const QStringList keys = object.keys();
    return QSet<QString>(keys.cbegin(), keys.cend()) == QSet<QString>(names.cbegin(), names.cend());
}

bool matches(const QString &text, const QString &pattern)
{
    return QRegularExpression(QRegularExpression::anchoredPattern(pattern)).match(text).hasMatch();
}

bool id(const QString &text)
{
    return matches(text, "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
}

bool checksum(const QJsonValue &value)
{
    return value.isString() && matches(value.toString(), "^[0-9a-f]{64}$");
}

bool integer(const QJsonValue &value, double minimum, double maximum = 9007199254740991.0)
{
    const double number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && number >= minimum
        && number <= maximum && std::floor(number) == number;
}

bool timestamp(const QJsonValue &value)
{
    const QString text = value.toString();
    return matches(text, "^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3}Z$")
        && QDateTime::fromString(text, Qt::ISODateWithMs).isValid();
}

bool path(const QString &text)
{
    if (text.isEmpty() || text.startsWith('/') || text.contains('\\')) return false;
    for (const QChar character : text)
        if (character.unicode() < 32 || character.unicode() == 127) return false;
    for (const QString &part : text.split('/'))
        if (part.isEmpty() || part == "." || part == "..") return false;
    return true;
}

bool noFileParents(const QSet<QString> &paths)
{
    for (const QString &name : paths) {
        int separator = name.indexOf('/');
        while (separator >= 0) {
            if (paths.contains(name.left(separator))) return false;
            separator = name.indexOf('/', separator + 1);
        }
    }
    return true;
}

} // namespace

bool IncrementalManifest::validate(const QByteArray &json, const QString &setId,
    const QString &snapshotId, QString *error)
{
    if (error) error->clear();
    const auto invalid = [&](const QString &reason) {
        if (error) *error = "Invalid incremental manifest: " + reason;
        return false;
    };
    const QJsonDocument document = QJsonDocument::fromJson(json);
    const QJsonObject root = document.object();
    if (!document.isObject() || !fields(root, {"version", "application", "set_id", "origin_computer_id",
            "owner_computer_id", "ownership_epoch", "snapshot_id", "created_at", "status", "archives", "entries"})
        || root.value("version") != QJsonValue(4) || root.value("application") != QJsonValue("omacustos")
        || root.value("status") != QJsonValue("complete")
        || !id(setId) || !id(snapshotId) || root.value("set_id") != QJsonValue(setId)
        || root.value("snapshot_id") != QJsonValue(snapshotId)
        || !id(root.value("origin_computer_id").toString()) || !id(root.value("owner_computer_id").toString())
        || !integer(root.value("ownership_epoch"), 1) || !timestamp(root.value("created_at"))
        || !root.value("archives").isArray() || !root.value("entries").isArray())
        return invalid("unsupported schema or mismatched snapshot identity");

    QHash<QString, QHash<QString, QJsonObject>> inventories;
    for (const auto &value : root.value("archives").toArray()) {
        const QJsonObject archive = value.toObject();
        const QString objectId = archive.value("id").toString();
        const QJsonObject verification = archive.value("original_verification").toObject();
        if (!value.isObject() || !fields(archive, {"id", "remote", "size", "sha256", "members", "original_verification"})
            || !id(objectId) || inventories.contains(objectId)
            || archive.value("remote") != QJsonValue("objects/" + objectId + ".tar.gz")
            || !integer(archive.value("size"), 1) || !checksum(archive.value("sha256"))
            || !archive.value("members").isArray() || archive.value("members").toArray().isEmpty()
            || !fields(verification, {"method", "verified_at", "sha256"})
            || (verification.value("method") != QJsonValue("download-sha256")
                && verification.value("method") != QJsonValue("remote-sha256"))
            || !timestamp(verification.value("verified_at"))
            || verification.value("sha256") != archive.value("sha256"))
            return invalid("malformed archive inventory or original verification evidence");
        QHash<QString, QJsonObject> members;
        for (const auto &memberValue : archive.value("members").toArray()) {
            const QJsonObject member = memberValue.toObject();
            const QString name = member.value("path").toString();
            if (!memberValue.isObject() || !fields(member, {"path", "size", "sha256"})
                || !path(name) || members.contains(name) || !integer(member.value("size"), 0)
                || !checksum(member.value("sha256"))) return invalid("malformed physical member");
            members.insert(name, member);
        }
        const QStringList names = members.keys();
        if (!noFileParents(QSet<QString>(names.cbegin(), names.cend()))) return invalid("physical file/directory conflict");
        inventories.insert(objectId, members);
    }

    QSet<QString> destinations, usedArchives;
    for (const auto &value : root.value("entries").toArray()) {
        const QJsonObject entry = value.toObject();
        const QString restore = entry.value("restore").toString();
        const QString objectId = entry.value("archive").toString();
        const QString member = entry.value("member").toString();
        if (!value.isObject() || !fields(entry, {"restore", "archive", "member", "size", "sha256", "mode", "mtime_ns"})
            || !path(restore) || destinations.contains(restore) || !path(member)
            || !inventories.contains(objectId) || !inventories.value(objectId).contains(member)
            || !integer(entry.value("size"), 0) || !checksum(entry.value("sha256"))
            || !integer(entry.value("mode"), 0, 511)
            || !matches(entry.value("mtime_ns").toString(), "^(0|[1-9][0-9]{0,18})$")
            || entry.value("mtime_ns").toString().toULongLong() > 9223372036854775807ULL)
            return invalid("unsafe or unresolved logical file reference");
        const QJsonObject physical = inventories.value(objectId).value(member);
        if (entry.value("size") != physical.value("size") || entry.value("sha256") != physical.value("sha256"))
            return invalid("logical content differs from physical inventory");
        destinations.insert(restore);
        usedArchives.insert(objectId);
    }
    if (usedArchives.size() != inventories.size() || !noFileParents(destinations))
        return invalid("unused archive or restore file/directory conflict");
    return true;
}

#include "incrementalrestore.h"
#include "incrementalmanifest.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>

namespace {
bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

bool read(BackupProvider &provider, const QString &path, QJsonObject *object, QString *error)
{
    QTemporaryDir temporary;
    const QString local = temporary.filePath("record.json");
    if (!temporary.isValid() || !provider.download(path, local, error)) return false;
    QFile file(local);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, "Unable to read downloaded repository metadata.");
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (file.error() != QFileDevice::NoError || !document.isObject()) return fail(error, "Malformed repository metadata: " + path);
    *object = document.object();
    return true;
}

bool children(BackupProvider &provider, const QString &root, QVector<RemoteItem> *items, QString *error)
{
    if (!provider.list(root, items, error)) return false;
    QSet<QString> seen;
    for (const auto &item : *items) {
        if (!IncrementalManifest::safeRelativePath(item.name) || item.name.contains('/')
            || item.path != root + '/' + item.name || seen.contains(item.name))
            return fail(error, "Unsafe or ambiguous repository listing: " + root);
        seen.insert(item.name);
    }
    return true;
}

bool anchor(BackupProvider &provider, const QString &root, const QString &setId, QJsonObject *object, QString *error)
{
    if (!read(provider, root + "/repository.json", object, error)) return false;
    const auto epoch = object->value("ownership_epoch");
    const QSet<QString> expected {"version", "application", "coordination", "set_id", "origin_computer_id", "owner_computer_id", "ownership_epoch"};
    const auto keys = object->keys();
    if (QSet<QString>(keys.cbegin(), keys.cend()) != expected || object->value("version") != 1
        || object->value("application") != "omacustos" || object->value("coordination") != "manual-offline-v1"
        || object->value("set_id") != setId || !IncrementalManifest::validId(object->value("origin_computer_id").toString())
        || !IncrementalManifest::validId(object->value("owner_computer_id").toString())
        || !epoch.isDouble() || epoch.toDouble() < 1 || epoch.toDouble() > 9007199254740991.0
        || epoch.toDouble() != double(epoch.toInteger())) return fail(error, "Invalid repository identity or ownership record.");
    return true;
}
}

bool IncrementalRestore::snapshotLocation(const QString &root, QString *setRoot, QString *setId, QString *snapshotId)
{
    const auto parts = root.split('/');
    if (root != QDir::cleanPath(root) || root.contains('\\') || root.contains(QChar::Null)
        || parts.contains("..") || parts.size() < 4 || parts.at(parts.size() - 4) != "sets"
        || parts.at(parts.size() - 2) != "snapshots"
        || !IncrementalManifest::validId(parts.at(parts.size() - 3)) || !IncrementalManifest::validId(parts.last())) return false;
    *setId = parts.at(parts.size() - 3);
    *snapshotId = parts.last();
    *setRoot = root.left(root.size() - snapshotId->size() - QString("/snapshots/").size());
    return true;
}

bool IncrementalRestore::verifyCopy(BackupProvider &provider, const QString &root, const QString &expectedSetId,
    RemoteCopy *copy, QString *error, const std::function<bool()> &cancelled)
{
    QString setRoot, setId, snapshotId;
    if (!copy || !snapshotLocation(root, &setRoot, &setId, &snapshotId)
        || (!expectedSetId.isEmpty() && setId != expectedSetId)) return fail(error, "Invalid selected shared snapshot location.");
    QJsonObject control, manifest;
    if (cancelled && cancelled()) return false;
    if (!anchor(provider, setRoot, setId, &control, error)
        || !read(provider, root + "/manifest.json", &manifest, error)) return false;
    if (!IncrementalManifest::validate(QJsonDocument(manifest).toJson(), setId, snapshotId, error)) return false;
    if (manifest.value("origin_computer_id") != control.value("origin_computer_id")
        || manifest.value("ownership_epoch").toInteger() > control.value("ownership_epoch").toInteger()
        || (manifest.value("ownership_epoch") == control.value("ownership_epoch")
            && manifest.value("owner_computer_id") != control.value("owner_computer_id")))
        return fail(error, "Snapshot provenance conflicts with its repository.");
    *copy = {root, root + "/manifest.json", control.value("origin_computer_id").toString(), setId, setId,
        snapshotId, "complete", QDateTime::fromString(manifest.value("created_at").toString(), Qt::ISODateWithMs), {}, {}, {}};
    QHash<QString, BackupArchive> archives;
    QSet<QString> unavailable;
    for (const auto &value : manifest.value("archives").toArray()) {
        if (cancelled && cancelled()) return false;
        const auto object = value.toObject();
        BackupArchive archive;
        archive.id = object.value("id").toString();
        archive.remotePath = setRoot + '/' + object.value("remote").toString();
        archive.size = object.value("size").toInteger();
        archive.checksum = QByteArray::fromHex(object.value("sha256").toString().toLatin1());
        for (const auto &value : object.value("members").toArray()) {
            const auto member = value.toObject();
            archive.members.append(member.value("path").toString());
            archive.inventory.append({member.value("path").toString(), member.value("size").toInteger(),
                QByteArray::fromHex(member.value("sha256").toString().toLatin1())});
        }
        RemoteFile metadata;
        if (!provider.inspect(archive.remotePath, &metadata, error) || metadata.path != archive.remotePath
            || metadata.size != archive.size || (provider.trustworthyContentChecksums() && metadata.checksum != archive.checksum))
            unavailable.insert(archive.id);
        archives.insert(archive.id, archive);
    }
    for (const auto &value : manifest.value("entries").toArray()) {
        const auto object = value.toObject();
        BackupEntry entry;
        entry.restorePath = object.value("restore").toString();
        entry.sourcePath = entry.restorePath;
        entry.archive = archives.value(object.value("archive").toString());
        entry.remotePath = entry.archive.remotePath;
        entry.memberPath = object.value("member").toString();
        entry.size = object.value("size").toInteger();
        entry.checksum = QByteArray::fromHex(object.value("sha256").toString().toLatin1());
        entry.mode = object.value("mode").toInt();
        entry.mtimeNs = object.value("mtime_ns").toString().toLongLong();
        if (unavailable.contains(entry.archive.id)) copy->unavailableItems.append(entry.restorePath);
        else copy->entries.append(entry);
    }
    return true;
}

bool IncrementalRestore::discover(BackupProvider &provider, const QString &remoteRoot, QVector<RemoteCopy> *copies, QString *error)
{
    if (!copies || remoteRoot.isEmpty() || remoteRoot != QDir::cleanPath(remoteRoot)
        || remoteRoot.split('/').contains("..") || remoteRoot.contains('\\') || remoteRoot.contains(QChar::Null))
        return fail(error, "An exact remote discovery root is required.");
    copies->clear();
    const QString sets = QDir(remoteRoot).filePath("sets");
    QVector<RemoteItem> repositories;
    if (!children(provider, sets, &repositories, error)) return false;
    for (const auto &repository : repositories) {
        if (!repository.directory || !IncrementalManifest::validId(repository.name))
            return fail(error, "Unexpected repository namespace entry.");
        QJsonObject control;
        if (!anchor(provider, repository.path, repository.name, &control, error)) return false;
        QVector<RemoteItem> snapshots;
        const QString root = repository.path + "/snapshots";
        const auto presence = provider.lookup(root, error);
        if (presence == RemoteLookup::Missing) continue;
        if (presence == RemoteLookup::Unknown || !children(provider, root, &snapshots, error)) return false;
        for (const auto &snapshot : snapshots) {
            if (!snapshot.directory || !IncrementalManifest::validId(snapshot.name))
                return fail(error, "Unexpected snapshot namespace entry.");
            const auto presence = provider.lookup(snapshot.path + "/manifest.json", error);
            if (presence == RemoteLookup::Missing) continue;
            if (presence == RemoteLookup::Unknown) return false;
            RemoteCopy copy;
            QString problem;
            if (!verifyCopy(provider, snapshot.path, repository.name, &copy, &problem)) {
                copy = {snapshot.path, snapshot.path + "/manifest.json", {}, repository.name, repository.name,
                    snapshot.name, "damaged", {}, {}, {}, {problem}};
            }
            copies->append(copy);
        }
    }
    std::sort(copies->begin(), copies->end(), [](const RemoteCopy &a, const RemoteCopy &b) {
        return a.createdAt == b.createdAt ? a.rootPath < b.rootPath : a.createdAt > b.createdAt;
    });
    return true;
}

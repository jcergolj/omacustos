#include "backupmanifest.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QDateTime>
#include <QtMath>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace {

bool stringArray(const QJsonValue &value)
{
    if (!value.isArray()) {
        return false;
    }
    for (const QJsonValue &item : value.toArray()) {
        if (!item.isString() || item.toString().isEmpty()) {
            return false;
        }
    }
    return true;
}

bool validChecksum(const QString &value)
{
    if (value.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256) * 2) {
        return false;
    }
    return std::all_of(value.cbegin(), value.cend(), [](const QChar character) {
        const QChar lower = character.toLower();
        return (character >= QChar('0') && character <= QChar('9'))
            || (lower >= QChar('a') && lower <= QChar('f'));
    });
}

bool completeState(const QSet<QString> &entryPaths, const QSet<QString> &expected,
    const QStringList &failed, const QVector<BackupIssue> &issues)
{
    return failed.isEmpty() && issues.isEmpty() && entryPaths == expected;
}

// Both locally constructed and downloaded manifests pass through this boundary.
bool decode(const QJsonDocument &document, QVector<BackupEntry> *entries, BackupManifestInfo *info, QString *error)
{
    const QJsonObject root = document.object();
    const int version = root.value(QStringLiteral("version")).toInt();
    if (!document.isObject() || (version != 1 && version != 2)) {
        if (error != nullptr) {
            *error = QStringLiteral("The backup manifest is malformed or unsupported.");
        }

        return false;
    }
    QVector<BackupIssue> issues;
    if (root.contains(QStringLiteral("issues"))) {
        const QJsonValue value = root.value(QStringLiteral("issues"));
        if (!value.isArray()) {
            if (error != nullptr) *error = QStringLiteral("The backup manifest is malformed or unsupported.");
            return false;
        }
        for (const QJsonValue &item : value.toArray()) {
            const QJsonObject issue = item.toObject();
            if (!item.isObject() || !issue.value(QStringLiteral("path")).isString()
                || !issue.value(QStringLiteral("phase")).isString()
                || !issue.value(QStringLiteral("reason")).isString()) {
                if (error != nullptr) *error = QStringLiteral("The backup manifest is malformed or unsupported.");
                return false;
            }
            issues.append({issue.value(QStringLiteral("path")).toString(),
                issue.value(QStringLiteral("phase")).toString(), issue.value(QStringLiteral("reason")).toString()});
        }
    }

    if (version == 2 && (root.value(QStringLiteral("application")).toString() != QStringLiteral("omacustos")
            || root.value(QStringLiteral("computer")).toString().isEmpty()
            || root.value(QStringLiteral("set_id")).toString().isEmpty()
            || root.value(QStringLiteral("copy_id")).toString().isEmpty()
            || (root.contains(QStringLiteral("set_name")) && !root.value(QStringLiteral("set_name")).isString())
            || !QDateTime::fromString(root.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs).isValid()
            || (root.value(QStringLiteral("status")).toString() != QStringLiteral("complete")
                && root.value(QStringLiteral("status")).toString() != QStringLiteral("incomplete"))
            || !stringArray(root.value(QStringLiteral("expected")))
            || !stringArray(root.value(QStringLiteral("failed"))))) {
        if (error != nullptr) {
            *error = QStringLiteral("The backup manifest is malformed or unsupported.");
        }
        return false;
    }

    const QJsonValue entriesValue = root.value(QStringLiteral("entries"));
    if (!entriesValue.isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("The backup manifest does not contain an entries list.");
        }
        return false;
    }
    const QJsonArray manifestEntries = entriesValue.toArray();
    QStringList expectedItems;
    QStringList failedItems;
    QSet<QString> expectedSet;
    QSet<QString> failedSet;
    if (version == 2) {
        for (const QJsonValue &item : root.value(QStringLiteral("expected")).toArray()) {
            expectedItems.append(item.toString());
        }
        for (const QJsonValue &item : root.value(QStringLiteral("failed")).toArray()) {
            failedItems.append(item.toString());
        }
        expectedSet = QSet<QString>(expectedItems.cbegin(), expectedItems.cend());
        failedSet = QSet<QString>(failedItems.cbegin(), failedItems.cend());
        if (expectedSet.size() != expectedItems.size() || failedSet.size() != failedItems.size()) {
            if (error != nullptr) {
                *error = QStringLiteral("The backup manifest is malformed or unsupported.");
            }
            return false;
        }
    }
    if (info != nullptr) {
        info->version = version;
        info->application = root.value(QStringLiteral("application")).toString();
        info->computerName = root.value(QStringLiteral("computer")).toString();
        info->setId = root.value(QStringLiteral("set_id")).toString();
        info->setName = root.value(QStringLiteral("set_name")).toString();
        info->copyId = root.value(QStringLiteral("copy_id")).toString();
        info->createdAt = QDateTime::fromString(root.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs);
        info->status = root.value(QStringLiteral("status")).toString(version == 1 ? QStringLiteral("complete") : QString());
        info->expectedItems = expectedItems;
        info->failedItems = failedItems;
        info->issues = issues;
    }
    QSet<QString> entryPaths;
    QSet<QString> remotePaths;
    for (const QJsonValue &value : manifestEntries) {
        if (!value.isObject()) {
            if (error != nullptr) {
                *error = QStringLiteral("The backup manifest contains an unsafe path.");
            }

            return false;
        }

        const QJsonObject object = value.toObject();
        const QString source = object.value(QStringLiteral("source")).toString();
        const QString remote = object.value(QStringLiteral("remote")).toString();
        const QString restore = object.value(QStringLiteral("restore")).toString();
        const QJsonValue sizeValue = object.value(QStringLiteral("size"));
        const QString checksumText = object.value(QStringLiteral("sha256")).toString();
        const QByteArray checksum = QByteArray::fromHex(checksumText.toLatin1());
        const QStringList remoteParts = remote.split('/', Qt::KeepEmptyParts);
        const bool containsParentSegment = std::any_of(
            remoteParts.cbegin(), remoteParts.cend(), [](const QString &part) {
                return part == QStringLiteral("..");
            });
        const QString restorePath = restore.isEmpty() ? QFileInfo(source).fileName() : restore;
        const QStringList restoreParts = restorePath.split('/', Qt::KeepEmptyParts);
        const bool unsafeRestorePath = restorePath.isEmpty() || restorePath.startsWith('/')
            || restorePath == QStringLiteral(".") || QDir::cleanPath(restorePath) != restorePath
            || std::any_of(restoreParts.cbegin(), restoreParts.cend(), [](const QString &part) {
                return part == QStringLiteral("..");
            });
        if (source.isEmpty() || remote.isEmpty() || unsafeRestorePath
            || (object.contains(QStringLiteral("restore")) && !object.value(QStringLiteral("restore")).isString())
            || containsParentSegment || !sizeValue.isDouble()
            || !qIsFinite(sizeValue.toDouble()) || sizeValue.toDouble() < 0
            || sizeValue.toDouble() >= 9223372036854775808.0
            || sizeValue.toDouble() != std::floor(sizeValue.toDouble())
            || !validChecksum(checksumText)
            || (version == 2 && (!expectedSet.contains(restorePath) || failedSet.contains(restorePath)))) {
            if (error != nullptr) {
                *error = QStringLiteral("The backup manifest contains an unsafe path.");
            }

            return false;
        }

        if (entryPaths.contains(restorePath) || remotePaths.contains(QDir::cleanPath(remote))) {
            if (error != nullptr) *error = QStringLiteral("The backup manifest is malformed or unsupported.");
            return false;
        }
        entryPaths.insert(restorePath);
        remotePaths.insert(QDir::cleanPath(remote));
        entries->append({source, remote, static_cast<qint64>(sizeValue.toDouble()), checksum, restorePath});
    }

    if (version == 2) {
        if (root.value(QStringLiteral("status")).toString() == QStringLiteral("complete")
            && !completeState(entryPaths, expectedSet, failedItems, issues)) {
            if (error != nullptr) {
                *error = QStringLiteral("The backup manifest is malformed or unsupported.");
            }
            return false;
        }
    }

    return true;
}

}

bool BackupManifest::write(const QString &path, const BackupManifestDraft &draft, QString *error)
{
    if (error != nullptr) error->clear();
    QJsonArray entries;
    QSet<QString> entryPaths;
    for (const BackupEntry &entry : draft.verifiedEntries) {
        const QString restorePath = entry.restorePath.isEmpty() ? QFileInfo(entry.sourcePath).fileName() : entry.restorePath;
        entryPaths.insert(restorePath);
        entries.append(QJsonObject {
            {QStringLiteral("source"), entry.sourcePath}, {QStringLiteral("remote"), entry.remotePath},
            {QStringLiteral("restore"), restorePath}, {QStringLiteral("size"), entry.size},
            {QStringLiteral("sha256"), QString::fromLatin1(entry.checksum.toHex())},
        });
    }
    QJsonArray issues;
    for (const BackupIssue &issue : draft.issues) {
        issues.append(QJsonObject {{QStringLiteral("path"), issue.path},
            {QStringLiteral("phase"), issue.phase}, {QStringLiteral("reason"), issue.reason}});
    }
    const BackupCopyMetadata &metadata = draft.metadata;
    QJsonObject root {
        {QStringLiteral("version"), metadata.copyId.isEmpty() ? 1 : 2},
        {QStringLiteral("entries"), entries}, {QStringLiteral("issues"), issues},
    };
    if (!metadata.copyId.isEmpty()) {
        const QSet<QString> expected(draft.expectedItems.cbegin(), draft.expectedItems.cend());
        root.insert(QStringLiteral("application"), QStringLiteral("omacustos"));
        root.insert(QStringLiteral("computer"), metadata.computerName);
        root.insert(QStringLiteral("set_id"), metadata.setId);
        root.insert(QStringLiteral("set_name"), metadata.setName);
        root.insert(QStringLiteral("copy_id"), metadata.copyId);
        root.insert(QStringLiteral("created_at"), metadata.createdAt.toString(Qt::ISODateWithMs));
        root.insert(QStringLiteral("status"), completeState(entryPaths, expected, draft.failedItems, draft.issues)
            ? QStringLiteral("complete") : QStringLiteral("incomplete"));
        root.insert(QStringLiteral("expected"), QJsonArray::fromStringList(draft.expectedItems));
        root.insert(QStringLiteral("failed"), QJsonArray::fromStringList(draft.failedItems));
    }
    const QJsonDocument document(root);
    QVector<BackupEntry> validatedEntries;
    if (!decode(document, &validatedEntries, nullptr, error)) return false;

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const QByteArray contents = document.toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit()) {
        if (error != nullptr) *error = QStringLiteral("Unable to write the backup manifest.");
        return false;
    }
    return true;
}

bool BackupManifest::load(const QString &path, QVector<BackupEntry> *entries, QString *error)
{
    return load(path, entries, nullptr, error);
}

bool BackupManifest::load(const QString &path, QVector<BackupEntry> *entries, BackupManifestInfo *info, QString *error)
{
    if (info != nullptr) *info = {};
    if (entries == nullptr) {
        if (error != nullptr) *error = QStringLiteral("A destination for manifest entries is required.");
        return false;
    }
    entries->clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = QStringLiteral("The backup manifest could not be opened.");
        return false;
    }
    const QByteArray contents = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        if (error != nullptr) *error = QStringLiteral("The backup manifest could not be read.");
        return false;
    }
    QVector<BackupEntry> loadedEntries;
    BackupManifestInfo loadedInfo;
    if (!decode(QJsonDocument::fromJson(contents), &loadedEntries, &loadedInfo, error)) return false;
    *entries = loadedEntries;
    if (info != nullptr) *info = loadedInfo;
    return true;
}

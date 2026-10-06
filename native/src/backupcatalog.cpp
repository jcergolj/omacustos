#include "backupcatalog.h"

#include "backupmanifest.h"
#include "payloadmetadatapolicy.h"
#include "remotemetadatacache.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>

namespace {

bool inspectFolder(BackupProvider &provider, const QString &path, QVector<RemoteItem> *items, QString *error)
{
    return provider.list(path, items, error);
}

bool inside(const QString &path, const QString &root)
{
    const QString cleanPath = QDir::cleanPath(path);
    const QString cleanRoot = QDir::cleanPath(root);
    return cleanPath == cleanRoot || (cleanRoot == QStringLiteral("/")
        ? cleanPath.startsWith('/')
        : cleanPath.startsWith(cleanRoot + QDir::separator()));
}

void warning(QString *error, const QString &message)
{
    if (error != nullptr && error->isEmpty()) {
        *error = message;
    }
}

bool visit(BackupProvider &provider, const QString &path, const QString &rootPath,
    QVector<RemoteCopy> *copies, QSet<QString> *visited, QString *error)
{
    const QString cleanPath = QDir::cleanPath(path);
    if (visited->contains(cleanPath)) {
        return true;
    }
    visited->insert(cleanPath);

    QVector<RemoteItem> items;
    if (!inspectFolder(provider, path, &items, error)) {
        return false;
    }

    for (const RemoteItem &item : items) {
        if (!inside(item.path, rootPath)) {
            warning(error, QStringLiteral("The provider returned a path outside the discovery root: %1").arg(item.path));
            continue;
        }
        if (item.directory) {
            if (!visit(provider, item.path, rootPath, copies, visited, error)) {
                return false;
            }
            continue;
        }
        if (item.name != QStringLiteral("manifest.json")) {
            continue;
        }

        RemoteCopy copy;
        QString copyError;
        if (!BackupCatalog::verifyCopy(provider, QFileInfo(item.path).path(), {}, &copy, &copyError)) {
            warning(error, copyError);
            continue;
        }
        warning(error, copyError);
        copies->append(copy);
    }
    return true;
}

}

bool BackupCatalog::listCopies(BackupProvider &provider, const QString &backupFolder, QVector<RemoteCopy> *copies, QString *error)
{
    if (copies == nullptr || backupFolder.trimmed().isEmpty()) {
        warning(error, QStringLiteral("A remote backup folder and destination for copies are required."));
        return false;
    }
    copies->clear();
    const QString root = QDir::cleanPath(backupFolder);
    QVector<RemoteItem> folders;
    if (!provider.list(root, &folders, error)) {
        return false;
    }
    QSet<QString> seen;
    for (const RemoteItem &folder : folders) {
        const QString path = QDir::cleanPath(folder.path);
        if (!folder.directory || path != folder.path || QFileInfo(path).path() != root || seen.contains(path)) {
            continue;
        }
        seen.insert(path);
        copies->append({path, QDir(path).filePath(QStringLiteral("manifest.json")),
            QFileInfo(QFileInfo(root).path()).fileName(), {}, QFileInfo(root).fileName(),
            QFileInfo(path).fileName(), QStringLiteral("not verified"), {}, {}, {}, {}});
    }
    // Generated copy IDs begin with a UTC timestamp, so newest copies appear first.
    std::sort(copies->begin(), copies->end(), [](const RemoteCopy &left, const RemoteCopy &right) {
        return left.copyId > right.copyId;
    });
    return true;
}

bool BackupCatalog::verifyCopy(BackupProvider &provider, const QString &copyFolder, const QString &expectedSetId,
    RemoteCopy *copy, QString *error, const std::function<bool()> &cancelled)
{
    if (cancelled && cancelled()) return false;
    if (copy == nullptr || copyFolder.trimmed().isEmpty()) {
        warning(error, QStringLiteral("A remote copy folder and destination are required."));
        return false;
    }
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        warning(error, QStringLiteral("Unable to create a temporary manifest folder."));
        return false;
    }
    const QString root = QDir::cleanPath(copyFolder);
    const QString remoteManifest = QDir(root).filePath(QStringLiteral("manifest.json"));
    const QString localManifest = temporary.filePath(QStringLiteral("manifest.json"));
    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    if (!provider.download(remoteManifest, localManifest, error)) {
        warning(error, QStringLiteral("The remote manifest %1 is unavailable.").arg(remoteManifest));
        return false;
    }
    if (cancelled && cancelled()) return false;
    if (!BackupManifest::load(localManifest, &entries, &info, error)) {
        warning(error, QStringLiteral("The remote manifest %1 is unavailable.").arg(remoteManifest));
        return false;
    }
    const bool legacy = info.version == 1
        && BackupManifest::identifyLegacyCopy(root, expectedSetId, entries, &info);
    if ((!legacy && ((info.version != 2 && info.version != 3) || info.application != QStringLiteral("omacustos")))
        || (!expectedSetId.isEmpty() && (info.setId != expectedSetId || info.copyId != QFileInfo(root).fileName()))) {
        warning(error, QStringLiteral("The remote manifest %1 is not the expected OmaCustos backup copy.").arg(remoteManifest));
        return false;
    }
    *copy = {root, remoteManifest, info.computerName, info.setId, info.setName, info.copyId,
        info.status, info.createdAt, {}, {}, info.failedItems};
    RemoteMetadataCache directoryMetadata(provider);
    QHash<QString, bool> verifiedArchives;
    for (const BackupEntry &entry : entries) {
        // Provider calls remain serial; superseded browsing stops between calls.
        if (cancelled && cancelled()) return false;
        if (!inside(entry.remotePath, root)) {
            warning(error, QStringLiteral("The remote manifest %1 points outside its copy.").arg(remoteManifest));
            copy->unavailableItems.append(entry.restorePath);
            continue;
        }
        RemoteFile remoteFile;
        QString providerError;
        if (!entry.archive.id.isEmpty() && verifiedArchives.contains(entry.archive.id)) {
            if (verifiedArchives.value(entry.archive.id)) copy->entries.append(entry);
            else copy->unavailableItems.append(entry.restorePath);
            continue;
        }
        const QString parent = QFileInfo(entry.remotePath).path();
        if (directoryMetadata.loadDirectory(parent, &providerError) && cancelled && cancelled()) return false;
        const bool listed = directoryMetadata.lookup(entry.remotePath, &remoteFile);
        const bool valid = (listed || provider.inspect(entry.remotePath, &remoteFile, &providerError))
            && PayloadMetadataPolicy::matchesAfterTransfer(remoteFile,
                entry.archive.id.isEmpty() ? entry.size : entry.archive.size,
                entry.archive.id.isEmpty() ? entry.checksum : entry.archive.checksum);
        if (!entry.archive.id.isEmpty()) verifiedArchives.insert(entry.archive.id, valid);
        if (!valid) {
            copy->unavailableItems.append(entry.restorePath);
            continue;
        }
        copy->entries.append(entry);
    }
    return true;
}

bool BackupCatalog::discoverCopies(BackupProvider &provider, const QString &backupFolder, const QString &expectedSetId,
    QVector<RemoteCopy> *copies, QString *error)
{
    QVector<RemoteCopy> listed;
    if (copies == nullptr || !listCopies(provider, backupFolder, &listed, error)) return false;
    copies->clear();
    for (const RemoteCopy &candidate : listed) {
        RemoteCopy copy;
        QString copyError;
        if (verifyCopy(provider, candidate.rootPath, expectedSetId, &copy, &copyError)) {
            copies->append(copy);
        }
        warning(error, copyError);
    }
    std::sort(copies->begin(), copies->end(), [](const RemoteCopy &left, const RemoteCopy &right) {
        return left.createdAt > right.createdAt;
    });
    return true;
}

bool BackupCatalog::discover(BackupProvider &provider, const QString &remoteRoot, QVector<RemoteCopy> *copies, QString *error)
{
    if (copies == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("A destination for remote copies is required.");
        }
        return false;
    }
    copies->clear();
    if (remoteRoot.trimmed().isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("A remote backup folder is required.");
        }
        return false;
    }

    const QString normalizedRoot = QDir::cleanPath(remoteRoot);
    QSet<QString> visited;
    if (!visit(provider, normalizedRoot, normalizedRoot, copies, &visited, error)) {
        copies->clear();
        return false;
    }
    std::sort(copies->begin(), copies->end(), [](const RemoteCopy &left, const RemoteCopy &right) {
        return left.createdAt > right.createdAt;
    });
    return true;
}

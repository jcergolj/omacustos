#include "backupengine.h"
#include "backuparchive.h"
#include "backupcontinuation.h"
#include "backupstaging.h"
#include "payloadmetadatapolicy.h"
#include "remotemetadatacache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <optional>

namespace {
qint64 conservativeArchiveAllowance(qint64 logicalBytes, qint64 fileCount)
{
    // Incompressible gzip expansion, tar/PAX headers, and final stream overhead.
    return logicalBytes + logicalBytes / 100 + fileCount * 8192 + 65536;
}

bool within(const QString &path, const QString &root)
{
    return path == root || path.startsWith(root == "/" ? root : root + '/');
}

QString segment(const QString &name)
{
    QString result;
    for (QChar c : name.trimmed()) result += c.isLetterOrNumber() || c == '.' || c == '-' || c == '_' ? c : QChar('_');
    return result.isEmpty() || result == "." || result == ".." ? QStringLiteral("source") : result;
}

void reserveFile(const QString &path, QSet<QString> &files, QSet<QString> &directories)
{
    files.insert(path);
    QString parent = QFileInfo(path).path();
    while (parent != "." && !parent.isEmpty()) {
        directories.insert(parent);
        parent = QFileInfo(parent).path();
    }
}

QString allocateFilePath(const QString &desired, QSet<QString> &files, QSet<QString> &directories)
{
    const QStringList parts = desired.split('/');
    QString path;
    for (int index = 0; index < parts.size(); ++index) {
        const QString base = parts.at(index);
        QString name = base;
        int suffix = 1;
        const bool leaf = index == parts.size() - 1;
        const auto candidate = [&] { return path.isEmpty() ? name : path + '/' + name; };
        while (files.contains(candidate()) || (leaf && directories.contains(candidate())))
            name = base + '.' + QString::number(suffix++);
        path = candidate();
    }
    reserveFile(path, files, directories);
    return path;
}

bool hashFile(const QString &path, QByteArray *checksum, qint64 *size, const std::function<bool()> &stopped)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    *size = 0;
    while (!file.atEnd()) {
        if (stopped && stopped()) return false;
        const QByteArray chunk = file.read(1024 * 1024);
        if (file.error() != QFileDevice::NoError) return false;
        *size += chunk.size();
        hash.addData(chunk);
    }
    *checksum = hash.result();
    return true;
}
}

bool BackupEngine::backupBatches(const QStringList &sources, const QString &remoteRoot, const QStringList &exclusions,
    const BackupCopyMetadata &metadata, BackupProvider &provider, QString *manifestPath, QString *error,
    const std::function<void(const BackupProgress &)> &reportProgress, BackupResult *result, const BackupOptions &options) const
{
    BackupResult outcome;
    outcome.reported = true;
    const auto finished = qScopeGuard([&] { if (result) *result = outcome; });
    if (manifestPath) manifestPath->clear();
    if (error) error->clear();
    const auto fail = [&](const QString &message, bool storage = false) {
        outcome.interrupted = true;
        outcome.waitingForSpace = storage;
        if (error) *error = message;
        return false;
    };
    const auto stopped = [&] { return options.stopped && options.stopped(); };
    const bool archiveMode = options.singleArchive || options.boundedArchives;
    const auto checkpointFailed = [&] {
        outcome.interrupted = true;
        const QString reason = error ? error->toLower() : QString();
        outcome.waitingForSpace = reason.contains("disk quota exceeded") || reason.contains("no space left")
            || reason.contains("read-only file system");
        if (outcome.waitingForSpace && error)
            *error += QStringLiteral(" Free space/quota on the checkpoint disk before Resume.");
        return false;
    };
    const QString root = QDir::cleanPath(remoteRoot);
    if (root.isEmpty() || root == "." || root.split('/').contains("..") || options.stagingBudget <= 0
        || options.batchFileLimit <= 0 || (options.boundedArchives && options.archiveTargetBytes <= 0))
        return fail(QStringLiteral("The backup destination or staging budget is invalid."));

    BackupStaging staging;
    if (!staging.open(options.stagingDirectory, sources, error)) {
        outcome.interrupted = outcome.waitingForSpace = true;
        return false;
    }
    if (options.stagingReady && !options.stagingReady(staging.root(), error)) { outcome.interrupted = true; return false; }
    const bool recovering = !options.continuationDirectory.isEmpty()
        && QFileInfo::exists(QDir(options.continuationDirectory).filePath("identity.json"));
    BackupContinuation continuation(options.continuationDirectory);
    if (!continuation.open(root, metadata, error, archiveMode ? 3 : 2)) {
        return checkpointFailed();
    }
    QSet<QString> prefixes;
    for (const QString &prefix : continuation.roots) prefixes.insert(prefix);
    for (const auto &entry : continuation.mappings)
        prefixes.insert(entry.remotePath.mid(root.size() + 1).section('/', 0, 0));
    // New roots cannot occupy a directory already selected beneath an original
    // unprefixed root, even if those files have not reached a checkpoint yet.
    for (auto it = continuation.roots.cbegin(); it != continuation.roots.cend(); ++it) {
        if (it.value().isEmpty() && QFileInfo(it.key()).isDir()) {
            const auto names = QDir(it.key()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
            for (const QString &name : names) prefixes.insert(name);
        }
    }
    for (const QString &source : sources) {
        const QString absolute = QFileInfo(source).absoluteFilePath();
        if (continuation.roots.contains(absolute)) continue;
        const QString base = segment(QFileInfo(source).fileName());
        QString prefix = continuation.roots.isEmpty() && sources.size() == 1 ? QString() : base;
        int suffix = 2;
        while (prefixes.contains(prefix)) prefix = base + '-' + QString::number(suffix++);
        continuation.roots.insert(absolute, prefix);
        prefixes.insert(prefix);
    }
    if (!continuation.saveRoots(error)) {
        return checkpointFailed();
    }

    BackupProgress progress;
    progress.phase = QStringLiteral("scanning");
    if (reportProgress) reportProgress(progress);
    const BackupPreview selection = scan(sources, exclusions, false, stopped, options.inclusions);
    if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
    for (const QString &path : selection.missingPaths)
        outcome.issues.append({path, "selection", "The source path does not exist."});
    for (const QString &path : selection.skippedPaths)
        outcome.issues.append({path, "selection", QFileInfo(path).isSymLink()
            ? QStringLiteral("Symbolic links are not backed up.") : QStringLiteral("The source path is unreadable or unsupported.")});
    if (selection.includedFiles.isEmpty()) {
        if (error) *error = QStringLiteral("The selected folder contains no regular files.");
        return false;
    }
    progress.totalFiles = selection.includedFiles.size();
    QHash<QString, QPair<qint64, qint64>> directoryPayloads;
    QHash<QString, QString> sourceOwnership;
    for (const QString &path : selection.includedFiles) progress.totalBytes += qMax(qint64(0), QFileInfo(path).size());
    for (const QString &path : selection.includedFiles) {
        for (const QString &source : sources) {
            const QString absolute = QFileInfo(source).absoluteFilePath();
            if (within(path, absolute)) { sourceOwnership.insert(path, absolute); break; }
        }
    }
    QStringList orderedFiles = selection.includedFiles;
    if (options.boundedArchives) {
        for (const QString &path : selection.includedFiles) {
            auto &payload = directoryPayloads[QFileInfo(path).absolutePath()];
            payload.first += qMax(qint64(0), QFileInfo(path).size());
            ++payload.second;
        }
        std::sort(orderedFiles.begin(), orderedFiles.end(), [&](const QString &left, const QString &right) {
            const QString leftSource = sourceOwnership.value(left), rightSource = sourceOwnership.value(right);
            if (leftSource != rightSource) return leftSource < rightSource;
            const QString leftDirectory = QFileInfo(left).absolutePath(), rightDirectory = QFileInfo(right).absolutePath();
            return leftDirectory == rightDirectory ? left < right : leftDirectory < rightDirectory;
        });
    }
    if (options.singleArchive) {
        if (progress.totalBytes > options.stagingBudget / 2)
            return fail(QStringLiteral("Waiting: this selection exceeds the internal single-archive staging allowance."), true);
        const qint64 archiveAllowance = conservativeArchiveAllowance(progress.totalBytes, progress.totalFiles);
        if (progress.totalBytes > options.stagingBudget - archiveAllowance
            || !staging.hasSpace(progress.totalBytes + archiveAllowance, error))
            return fail(QStringLiteral("Waiting: this selection exceeds the internal single-archive staging allowance."), true);
    }
    progress.failedItems = outcome.issues.size();
    BackupManifestDraft draft;
    draft.metadata = continuation.metadata;
    draft.failedItems = selection.missingPaths + selection.skippedPaths;
    QSet<QString> assigned {QStringLiteral("manifest.json")};
    // Archive objects occupy a reserved namespace; member identities remain
    // separate from object names, including sources named like the manifest.
    if (archiveMode) assigned.insert(QStringLiteral(".omacustos-archives"));
    QSet<QString> remoteDirectories, restoreFiles, restoreDirectories;
    // Keep historical mappings reserved, including deleted sources. New paths
    // must not overwrite another source's previous identity during reconciliation.
    for (const BackupEntry &entry : continuation.mappings) {
        reserveFile(archiveMode ? entry.memberPath : entry.remotePath.mid(root.size() + 1), assigned, remoteDirectories);
        reserveFile(entry.restorePath, restoreFiles, restoreDirectories);
    }
    provider.beginBackupOperation();
    const auto endOperation = qScopeGuard([&] { provider.endBackupOperation(); });
    QString providerError;
    if (!provider.ensureDirectory(root, &providerError)) return fail(providerError);
    if (archiveMode && !provider.ensureDirectory(QDir(root).filePath(".omacustos-archives"), &providerError))
        return fail(providerError);
    const bool directoryTransfer = provider.supportsDirectoryUpload()
        && std::any_of(sources.cbegin(), sources.cend(), [](const QString &path) { return QFileInfo(path).isDir(); });

    QVector<BackupEntry> batch;
    qint64 batchBytes = 0;
    QString batchSource;
    QString previousDirectory;
    bool oversizedBatch = false;
    const QString tree = QDir(staging.path()).filePath("payloads/" + QFileInfo(root).fileName());
    const auto report = [&](const QString &phase, const QString &path = QString(), qint64 size = 0) {
        progress.phase = phase;
        progress.currentFile = path;
        progress.currentFileBytes = size;
        if (reportProgress) reportProgress(progress);
    };
    const auto recordVerified = [&](const BackupEntry &entry) {
        draft.verifiedEntries.append(entry);
        ++outcome.verifiedFiles;
        outcome.verifiedBytes += entry.size;
        progress.verifiedFiles = outcome.verifiedFiles;
        progress.verifiedBytes = outcome.verifiedBytes;
    };
    bool reuseStorageFailure = false;
    const auto matchesRemote = [&](const BackupEntry &entry, QString *reason, bool oversizedArchive = false) {
        reuseStorageFailure = false;
        RemoteFile remote;
        if (!provider.inspect(entry.remotePath, &remote, reason)) {
            if (archiveMode) {
                // Confirm absence through a successful directory listing. A
                // transport/authentication error must keep the copy unfinished,
                // rather than authorizing a replacement upload.
                QVector<RemoteItem> items;
                QString listingError;
                if (provider.list(QFileInfo(entry.remotePath).path(), &items, &listingError)
                    && std::none_of(items.cbegin(), items.cend(), [&](const RemoteItem &item) {
                        return item.name == QFileInfo(entry.remotePath).fileName();
                    })) {
                    if (reason) reason->clear();
                }
            }
            return false;
        }
        if (remote.size != entry.size) return false;
        if (!remote.checksum.isEmpty()) return PayloadMetadataPolicy::matchesForReuse(remote, entry.size, entry.checksum);
        // A size-only provider cannot prove reuse. Download and hash when the
        // CLI exposes no checksum; never mistake same-size content for identity.
        const QString check = QDir(staging.path()).filePath("reuse-check");
        const auto remove = qScopeGuard([&] { QFile::remove(check); });
        if (archiveMode && entry.size > options.stagingBudget && !oversizedArchive) {
            reuseStorageFailure = true;
            if (reason) *reason = QStringLiteral("Waiting: archive verification exceeds the staging budget. Increase the allowance before Resume.");
            return false;
        }
        if (!staging.hasSpace(entry.size, reason)) { reuseStorageFailure = true; return false; }
        if (stopped()) return false;
        if (!provider.download(entry.remotePath, check, reason)) {
            const QString message = reason ? reason->toLower() : QString();
            reuseStorageFailure = message.contains("quota") || message.contains("no space left") || message.contains("read-only file system");
            if (reuseStorageFailure && reason) *reason += QStringLiteral(" Free space/quota or choose another staging disk before Resume.");
            return false;
        }
        QByteArray checksum;
        qint64 size;
        return hashFile(check, &checksum, &size, options.stopped) && size == entry.size && checksum == entry.checksum;
    };
    const auto flushBatch = [&]() {
        if (batch.isEmpty()) return true;
        if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
        if (archiveMode) {
            report("preparing");
            // Conservative incompressible tar/gzip allowance, alongside snapshots.
            const qint64 allowance = conservativeArchiveAllowance(batchBytes, batch.size());
            if ((!oversizedBatch && batchBytes > options.stagingBudget - allowance) || !staging.hasSpace(allowance, error))
                return fail(QStringLiteral("Waiting for archive staging storage: free space/quota or choose another staging disk."), true);
            const QString archiveName = "omacustos-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".tar.gz";
            const QString packed = QDir(staging.path()).filePath(archiveName);
            if (!BackupArchiveIO::pack(tree, root, packed, allowance, &batch, error, options.stopped)) {
                outcome.interrupted = true;
                const QString reason = error ? error->toLower() : QString();
                outcome.waitingForSpace = reason.contains("quota") || reason.contains("no space") || reason.contains("read-only")
                    || reason.contains("archive staging write failed") || reason.contains("archive staging flush failed")
                    || reason.contains("archive staging could not be opened");
                if (outcome.waitingForSpace && error)
                    *error += QStringLiteral(" Free space/quota or choose another staging disk before Resume.");
                return false;
            }
            BackupArchive archive {archiveName, QDir(root).filePath(".omacustos-archives/" + archiveName)};
            if (!hashFile(packed, &archive.checksum, &archive.size, options.stopped))
                return fail(QStringLiteral("The prepared archive could not be hashed."));
            if (archive.size > allowance) return fail(QStringLiteral("Waiting: compressed archive exceeded its staging allowance."), true);
            for (const auto &entry : batch) archive.members.append(entry.memberPath);
            for (BackupEntry &entry : batch) {
                entry.archive = archive;
                entry.remotePath = archive.remotePath;
            }
            report("prepared-checkpoint");
            if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
            if (!continuation.checkpoint(batch, error, false)) return checkpointFailed();
            report("uploading", sources.first(), archive.size);
            if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
            if (!provider.ensureDirectory(QFileInfo(archive.remotePath).path(), error)
                || !provider.upload(packed, archive.remotePath, error)) return fail(error ? *error : QStringLiteral("Archive upload failed."));
            if (stopped()) return fail(QStringLiteral("The backup was stopped."));
            report("verifying", sources.first(), archive.size);
            if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
            RemoteFile remote;
            if (!provider.inspect(archive.remotePath, &remote, error)
                || !PayloadMetadataPolicy::matchesAfterTransfer(remote, archive.size, archive.checksum))
                return fail(QStringLiteral("Remote archive verification failed."));
            report("checkpointing");
            if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
            if (!continuation.checkpoint(batch, error)) return checkpointFailed();
            draft.archives.append(archive);
            for (BackupEntry &entry : batch) {
                entry.archive = archive;
                entry.remotePath = archive.remotePath;
                recordVerified(entry);
            }
            progress.processedFiles += batch.size();
            progress.processedBytes += batchBytes;
            report("checkpointing");
            if (!QFile::remove(packed)) return fail(QStringLiteral("Unable to release compressed archive staging."), true);
            if (!staging.reset(error)) return fail(QStringLiteral("Unable to release archive staging."), true);
            batch.clear();
            batchBytes = 0;
            oversizedBatch = false;
            return true;
        }
        // Mapping identities must survive upload-before-checkpoint crashes too.
        // Prepared records never authorize reuse or count as verified progress.
        report("checkpointing");
        if (!continuation.checkpoint(batch, error, false)) return checkpointFailed();
        report(directoryTransfer ? QStringLiteral("uploading-folder") : QStringLiteral("uploading"),
            sources.size() == 1 ? sources.first() : root, batchBytes);
        bool uploaded = true;
        QString transferError;
        if (directoryTransfer) {
            uploaded = provider.uploadDirectory(tree, root, &transferError);
            if (!uploaded && !stopped())
                uploaded = provider.ensureDirectory(root, &transferError) && provider.uploadDirectory(tree, root, &transferError);
        } else {
            for (const BackupEntry &entry : batch) {
                report("uploading", entry.sourcePath, entry.size);
                const QString snapshot = QDir(tree).filePath(entry.remotePath.mid(root.size() + 1));
                bool success = provider.ensureDirectory(QFileInfo(entry.remotePath).path(), &transferError)
                    && provider.upload(snapshot, entry.remotePath, &transferError);
                if (!success && !stopped()) success = provider.ensureDirectory(QFileInfo(entry.remotePath).path(), &transferError)
                    && provider.upload(snapshot, entry.remotePath, &transferError);
                uploaded = uploaded && success;
                if (stopped()) break;
            }
        }
        if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
        progress.processedFiles += batch.size();
        progress.processedBytes += batchBytes;
        QVector<BackupEntry> verified;
        // The listing cache has batch lifetime. Payload staging remains intact
        // until all verified entries have been durably checkpointed.
        RemoteMetadataCache cache(provider);
        QString verificationError;
        for (const BackupEntry &entry : batch) {
            report("verifying", entry.sourcePath, entry.size);
            RemoteFile remote;
            cache.loadDirectory(QFileInfo(entry.remotePath).path(), &verificationError);
            const bool listed = cache.lookup(entry.remotePath, &remote);
            bool valid = (listed || provider.inspect(entry.remotePath, &remote, &verificationError))
                && PayloadMetadataPolicy::matchesAfterTransfer(remote, entry.size, entry.checksum);
            if (valid && !uploaded && !options.continuationDirectory.isEmpty() && remote.checksum.isEmpty()) {
                // A failed merge may have left old same-size content untouched.
                // Replace this item's snapshot allocation with a remote download
                // for positive checksum evidence, without exceeding batch bytes.
                QFile::remove(QDir(tree).filePath(entry.remotePath.mid(root.size() + 1)));
                valid = matchesRemote(entry, &verificationError);
            }
            if (valid) verified.append(entry);
            else if (options.continuationDirectory.isEmpty()) {
                outcome.issues.append({entry.sourcePath, "verifying", verificationError.isEmpty()
                    ? QStringLiteral("Remote verification failed.") : verificationError});
                draft.failedItems.append(entry.restorePath);
                ++progress.failedItems;
            }
            if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
        }
        report("checkpointing");
        if (!continuation.checkpoint(verified, error)) {
            return checkpointFailed();
        }
        for (const BackupEntry &entry : verified) recordVerified(entry);
        report("checkpointing");
        if (!uploaded || verified.size() != batch.size()) {
            // Partial transfers preserve positively verified work but never
            // publish a completed manifest or trigger retention.
            if (!options.continuationDirectory.isEmpty())
                return fail(!transferError.isEmpty() ? transferError : !verificationError.isEmpty()
                    ? verificationError : QStringLiteral("Remote payload verification failed; the unfinished copy will be retried."));
            if (!uploaded) {
                outcome.issues.append({sources.first(), "uploading", transferError.isEmpty()
                    ? QStringLiteral("The backup batch could not be uploaded.") : transferError});
                ++progress.failedItems;
            }
        }
        if (!staging.reset(error)) {
            outcome.interrupted = outcome.waitingForSpace = true;
            return false;
        }
        batch.clear();
        batchBytes = 0;
        report("preparing");
        return true;
    };

    // Reconcile whole groups from their latest prepared/verified records. A
    // prepared record only identifies candidate bytes: current local hashes and
    // checksum-grade remote evidence must precede a new durable checkpoint.
    QSet<QString> reusableSources;
    if (archiveMode && recovering) {
        const QSet<QString> selected(orderedFiles.cbegin(), orderedFiles.cend());
        QMap<QString, QVector<BackupEntry>> groups;
        for (const auto &entry : continuation.mappings) groups[entry.archive.id].append(entry);
        for (const auto &group : groups) {
            const BackupArchive archive = group.first().archive;
            bool valid = group.size() == archive.members.size();
            for (const auto &entry : group) {
                report("checking", entry.sourcePath, entry.size);
                QByteArray checksum;
                qint64 size;
                if (!selected.contains(entry.sourcePath)
                    || !hashFile(entry.sourcePath, &checksum, &size, options.stopped)
                    || size != entry.size || checksum != entry.checksum) valid = false;
                if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
            }
            if (!valid) continue;
            const BackupEntry object {QString(), archive.remotePath, archive.size, archive.checksum};
            report("verifying", archive.remotePath, archive.size);
            providerError.clear();
            const bool oversizedArchive = group.size() == 1
                && group.first().size + conservativeArchiveAllowance(group.first().size, 1) > options.stagingBudget;
            if (!matchesRemote(object, &providerError, oversizedArchive)) {
                if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
                if (!providerError.isEmpty()) return fail(providerError, reuseStorageFailure);
                continue;
            }
            if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
            const bool needsCheckpoint = std::any_of(group.cbegin(), group.cend(), [&](const BackupEntry &entry) {
                const auto verified = continuation.verified.constFind(entry.sourcePath);
                return verified == continuation.verified.cend() || verified->archive.id != archive.id;
            });
            if (needsCheckpoint) {
                report("checkpointing");
                if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
                if (!continuation.checkpoint(group, error)) return checkpointFailed();
            }
            draft.archives.append(archive);
            for (const auto &entry : group) {
                reusableSources.insert(entry.sourcePath);
                recordVerified(entry);
                ++progress.processedFiles;
                progress.processedBytes += entry.size;
            }
            report("checking");
        }
    }

    for (const QString &sourcePath : orderedFiles) {
        if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
        const QString sourceIdentity = sourceOwnership.value(sourcePath);
        if (options.boundedArchives && !batch.isEmpty() && sourceIdentity != batchSource)
            if (!flushBatch()) return false;
        batchSource = sourceIdentity;
        QString mapped;
        QString remoteMapped;
        const auto previous = continuation.verified.constFind(sourcePath);
        const auto mapping = continuation.mappings.constFind(sourcePath);
        if (mapping != continuation.mappings.cend()) {
            mapped = mapping->restorePath;
            remoteMapped = archiveMode ? mapping->memberPath : mapping->remotePath.mid(root.size() + 1);
        }
        else {
            const QString relative = QFileInfo(sourceIdentity).isDir() ? QDir(sourceIdentity).relativeFilePath(sourcePath) : QFileInfo(sourcePath).fileName();
            const QString prefix = continuation.roots.value(sourceIdentity);
            mapped = prefix.isEmpty() ? relative : QDir(prefix).filePath(relative);
            remoteMapped = allocateFilePath(mapped, assigned, remoteDirectories);
            mapped = allocateFilePath(mapped, restoreFiles, restoreDirectories);
        }
        draft.expectedItems.append(mapped);
        if (reusableSources.contains(sourcePath)) continue;
        qint64 plannedSize = qMax(qint64(0), QFileInfo(sourcePath).size());
        bool oversizedFile = false;
        if (options.boundedArchives) {
            const QString directory = QFileInfo(sourcePath).absolutePath();
            if (!batch.isEmpty() && directory != previousDirectory) {
                const auto payload = directoryPayloads.value(directory);
                const qint64 together = batchBytes + payload.first;
                // Keep a directory that fits an ordinary archive together when
                // moving it to the next group avoids splitting it. Tiny adjacent
                // directories still share an archive; large ones split by file.
                if (payload.first <= options.archiveTargetBytes
                    && payload.first + conservativeArchiveAllowance(payload.first, payload.second) <= options.stagingBudget
                    && (together > options.archiveTargetBytes
                        || together + conservativeArchiveAllowance(together, batch.size() + payload.second) > options.stagingBudget))
                    if (!flushBatch()) return false;
            }
            previousDirectory = directory;
            const qint64 singleWorking = plannedSize + conservativeArchiveAllowance(plannedSize, 1);
            oversizedFile = plannedSize > options.archiveTargetBytes || singleWorking > options.stagingBudget;
            const qint64 proposed = batchBytes + plannedSize;
            if (!batch.isEmpty() && (oversizedFile || proposed > options.archiveTargetBytes
                || proposed + conservativeArchiveAllowance(proposed, batch.size() + 1) > options.stagingBudget))
                if (!flushBatch()) return false;
            const qint64 required = batchBytes + plannedSize
                + conservativeArchiveAllowance(batchBytes + plannedSize, batch.size() + 1);
            // Free space is checked before snapshots, without counting on
            // compression. Only a single whole file may exceed the normal budget.
            if (!staging.hasSpace(required - batchBytes, error)) {
                return fail(QStringLiteral("Waiting for archive staging storage for %1: %2 bytes of additional working space required%3. Free space/quota or choose another staging disk. %4")
                    .arg(sourcePath).arg(required - batchBytes)
                    .arg(oversizedFile ? QStringLiteral(" (oversized file kept whole; exceeds normal archive allowance)") : QString())
                    .arg(error ? *error : QString()), true);
            }
            oversizedBatch = oversizedFile;
            if (oversizedFile) report("preparing-oversized-file", sourcePath, singleWorking);
        }
        if (!archiveMode && previous != continuation.verified.cend()) {
            report("checking", sourcePath, plannedSize);
            QByteArray checksum;
            qint64 size;
            if (hashFile(sourcePath, &checksum, &size, options.stopped)
                && size == previous->size && checksum == previous->checksum) {
                // Release prepared work before a potentially large reuse check
                // download, keeping temporary payload storage bounded.
                const BackupEntry candidate = *previous;
                if (!flushBatch()) return false;
                if (matchesRemote(candidate, &providerError)) {
                    recordVerified(candidate);
                    ++progress.processedFiles;
                    progress.processedBytes += candidate.size;
                    report("checking");
                    continue;
                }
            }
            if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
        }
        if (!archiveMode && recovering) {
            // Reconcile uploads missing from the last committed checkpoint.
            // Hash before staging so remote checksum evidence needs only one
            // payload allocation, including on size-only providers.
            report("checking", sourcePath, plannedSize);
            QByteArray checksum;
            qint64 size;
            RemoteFile remote;
            if (hashFile(sourcePath, &checksum, &size, options.stopped)
                && provider.inspect(QDir(root).filePath(remoteMapped), &remote, &providerError) && remote.size == size) {
                const BackupEntry candidate {sourcePath, QDir(root).filePath(remoteMapped), size, checksum, mapped};
                if (!flushBatch()) return false;
                if (matchesRemote(candidate, &providerError)) {
                    if (!continuation.checkpoint({candidate}, error)) return checkpointFailed();
                    recordVerified(candidate);
                    ++progress.processedFiles;
                    progress.processedBytes += size;
                    continue;
                }
            }
            if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
        }
        if (!archiveMode && !batch.isEmpty() && (batch.size() >= options.batchFileLimit || plannedSize > options.stagingBudget - batchBytes))
            if (!flushBatch()) return false;
        if (!staging.hasSpace(plannedSize, error)) {
            if (archiveMode) { outcome.interrupted = outcome.waitingForSpace = true; return false; }
            if (!flushBatch()) return false;
            if (!staging.hasSpace(plannedSize, error)) { outcome.interrupted = outcome.waitingForSpace = true; return false; }
        }
        report("staging", sourcePath, plannedSize);
        QFile source(sourcePath);
        const auto sourceFailure = [&](const QString &reason) {
            outcome.issues.append({sourcePath, "reading", reason});
            draft.failedItems.append(mapped);
            ++progress.failedItems;
            ++progress.processedFiles;
            progress.processedBytes += plannedSize;
            report("staging");
        };
        if (!source.open(QIODevice::ReadOnly)) {
            sourceFailure(QStringLiteral("The source file could not be read: %1").arg(source.errorString()));
            continue;
        }
        const QString snapshotPath = QDir(tree).filePath(remoteMapped);
        if (!QDir().mkpath(QFileInfo(snapshotPath).path())) return fail(QStringLiteral("Waiting for staging storage: unable to create payload directories. Choose another staging disk."), true);
        QFile snapshot(snapshotPath);
        if (!snapshot.open(QIODevice::WriteOnly)) return fail(QStringLiteral("Waiting for staging storage: %1. Choose another staging disk.").arg(snapshot.errorString()), true);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        bool readFailed = false;
        const qint64 allowance = options.boundedArchives ? plannedSize
            : options.singleArchive ? options.stagingBudget / 2 - batchBytes
            : qMax(options.stagingBudget, plannedSize) - batchBytes;
        while (!source.atEnd()) {
            if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
            const QByteArray chunk = source.read(1024 * 1024);
            if (source.error() != QFileDevice::NoError) { readFailed = true; break; }
            if (snapshot.size() > allowance - chunk.size())
                return fail(QStringLiteral("Waiting: source %1 grew beyond its prepared staging allowance. Resume to re-scan its size or choose another staging disk.").arg(sourcePath), true);
            if (snapshot.write(chunk) != chunk.size()) {
                // Filesystem free space says nothing about this user's quota.
                return fail(QStringLiteral("Waiting for staging storage while preparing %1 (%2 payload bytes required): %3. Free space/quota or choose another staging disk.")
                    .arg(sourcePath).arg(plannedSize).arg(snapshot.errorString()), true);
            }
            hash.addData(chunk);
        }
        if (readFailed) {
            snapshot.close();
            QFile::remove(snapshotPath);
            sourceFailure(QStringLiteral("The source file could not be read: %1").arg(source.errorString()));
            continue;
        }
        if (!snapshot.flush()) return fail(QStringLiteral("Waiting for staging storage while preparing %1 (%2 payload bytes required): %3. Free space/quota or choose another staging disk.")
            .arg(sourcePath).arg(plannedSize).arg(snapshot.errorString()), true);
        const qint64 size = snapshot.size();
        snapshot.close();
        if (!snapshot.setPermissions(QFileDevice::ReadOwner)) return fail(QStringLiteral("Unable to make the prepared payload immutable."), true);
        BackupEntry entry {sourcePath, QDir(root).filePath(remoteMapped), size, hash.result(), mapped};
        batch.append(entry);
        batchBytes += size;
        if ((options.boundedArchives && oversizedFile)
            || (!archiveMode && (size > options.stagingBudget || batch.size() >= options.batchFileLimit || batchBytes >= options.stagingBudget)))
            if (!flushBatch()) return false;
    }
    if (!flushBatch()) return false;
    if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
    progress.finalizing = true;
    report("finalizing");
    if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
    // The manifest itself lives on the staging disk, never system /tmp. Keep
    // the final local manifest for the caller, matching the engine API contract.
    QTemporaryDir manifestDirectory(QDir(options.retainLocalManifest ? staging.root() : staging.path()).filePath("manifest-XXXXXX"));
    if (!manifestDirectory.isValid()) return fail(QStringLiteral("Unable to prepare the final manifest on the staging disk."), true);
    const QString path = manifestDirectory.filePath("manifest.json");
    draft.issues = outcome.issues;
    if (!BackupManifest::write(path, draft, error)) { outcome.interrupted = true; return false; }
    const QString remoteManifest = QDir(root).filePath("manifest.json");
    if (!provider.upload(path, remoteManifest, &providerError)) return fail(providerError);
    RemoteFile remote;
    QByteArray checksum;
    qint64 size;
    if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
    if (!hashFile(path, &checksum, &size, options.stopped) || !provider.inspect(remoteManifest, &remote, &providerError)
        || !PayloadMetadataPolicy::matchesAfterTransfer(remote, size, checksum))
        return fail(providerError.isEmpty() ? QStringLiteral("Remote manifest verification failed.") : providerError);
    if (stopped()) return fail(QStringLiteral("The backup was stopped; its checkpoint is preserved."));
    outcome.manifestVerified = true;
    if (manifestPath && options.retainLocalManifest) { manifestDirectory.setAutoRemove(false); *manifestPath = path; }
    report("finalizing");
    if (!outcome.issues.isEmpty()) {
        if (error) *error = (outcome.verifiedFiles > 0 ? QStringLiteral("Backup incomplete: %1 files backed up · %2 items failed.")
            : QStringLiteral("Backup failed: %1 files backed up · %2 items failed.")).arg(outcome.verifiedFiles).arg(outcome.issues.size());
        return false;
    }
    return true;
}

#include "backupengine.h"
#include "backuparchive.h"
#include "localprovider.h"
#include "payloadmetadatapolicy.h"
#include "remotemetadatacache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QDirIterator>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QSaveFile>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <optional>

namespace {

QString effectiveRestorePath(const BackupEntry &entry)
{
    return entry.restorePath.isEmpty()
        ? (entry.sourcePath.startsWith('/') ? QFileInfo(entry.sourcePath).fileName() : entry.sourcePath)
        : entry.restorePath;
}

QString cleanAbsolutePath(const QString &path)
{
    return QFileInfo(path).absoluteFilePath();
}

bool isWithinPath(const QString &path, const QString &root)
{
    const QString cleanPath = QDir::cleanPath(path);
    const QString cleanRoot = QDir::cleanPath(root);

    return cleanPath == cleanRoot || (cleanRoot == QStringLiteral("/")
        ? cleanPath.startsWith('/')
        : cleanPath.startsWith(cleanRoot + QDir::separator()));
}

struct ExclusionRules {
    QSet<QString> names;
    QStringList paths;

    explicit ExclusionRules(const QStringList &exclusions)
    {
        for (const QString &exclusion : exclusions) {
            if (exclusion.trimmed().isEmpty()) continue;
            const QString rule = QDir::cleanPath(exclusion.trimmed());
            if (!rule.contains('/') && rule != QStringLiteral(".") && rule != QStringLiteral("..")) {
                names.insert(rule);
            } else {
                paths.append(QDir::cleanPath(cleanAbsolutePath(rule)));
            }
        }
    }
};

bool isExcluded(const QFileInfo &file, const ExclusionRules &rules)
{
    if (rules.names.isEmpty() && rules.paths.isEmpty()) return false;
    const QString path = QDir::cleanPath(file.absoluteFilePath());
    QStringList folders = file.absolutePath().split('/', Qt::SkipEmptyParts);
    if (file.isDir() || file.isSymLink()) {
        folders.append(file.fileName());
    }
    for (const QString &folder : folders) {
        if (rules.names.contains(folder)) return true;
    }
    return std::any_of(rules.paths.cbegin(), rules.paths.cend(), [&path](const QString &root) {
        return path == root || (root == QStringLiteral("/")
            ? path.startsWith('/') : path.startsWith(root + QDir::separator()));
    });
}

// Names match at every depth; paths are absolute or relative to each selected
// folder. Matching an ancestor folder includes its contents, never following links.
struct InclusionRules {
    struct Rule {
        QRegularExpression pattern;
        bool name;
        bool absolute;
    };
    QVector<Rule> rules;

    explicit InclusionRules(const QStringList &inclusions)
    {
        for (const QString &inclusion : inclusions) {
            if (inclusion.trimmed().isEmpty()) continue;
            const QString rule = QDir::cleanPath(inclusion.trimmed());
            rules.append({QRegularExpression(QRegularExpression::wildcardToRegularExpression(rule)),
                !rule.contains('/'), QDir::isAbsolutePath(rule)});
        }
    }

    bool matches(const QFileInfo &file, const QFileInfo &source) const
    {
        if (rules.isEmpty()) return true;
        const QString root = QDir::cleanPath(source.absoluteFilePath());
        const QDir base(source.isDir() ? root : source.absolutePath());
        QString path = QDir::cleanPath(file.absoluteFilePath());
        for (;;) {
            for (const Rule &rule : rules) {
                const QString candidate = rule.absolute ? path
                    : rule.name ? QFileInfo(path).fileName() : base.relativeFilePath(path);
                if (rule.pattern.match(candidate).hasMatch()) return true;
            }
            if (path == root) return false;
            const QString parent = QFileInfo(path).absolutePath();
            if (parent == path) return false;
            path = parent;
        }
    }
};

QString remoteSegment(QString value)
{
    value = value.trimmed();
    QString sanitized;
    for (const QChar character : value) {
        sanitized.append(character.isLetterOrNumber() || character == '.' || character == '_' || character == '-'
                ? character
                : QChar('_'));
    }

    return sanitized.isEmpty() ? QStringLiteral("source") : sanitized;
}

bool hasParentPathSegment(const QString &path)
{
    const QStringList parts = path.split('/', Qt::KeepEmptyParts);
    return std::any_of(parts.cbegin(), parts.cend(), [](const QString &part) {
        return part == QStringLiteral("..");
    });
}

bool copyAndHash(QFile &source, QFileDevice &snapshot, QByteArray *checksum, const std::function<bool()> &stopped = {})
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!source.atEnd()) {
        if (stopped && stopped()) return false;
        const QByteArray chunk = source.read(1024 * 1024);
        if ((chunk.isEmpty() && source.error() != QFileDevice::NoError)
            || snapshot.write(chunk) != chunk.size()) {
            return false;
        }
        hash.addData(chunk);
    }
    if (!snapshot.flush()) {
        return false;
    }
    *checksum = hash.result();
    return true;
}

struct StagedPayload {
    QString path;
    qint64 size;
    QByteArray checksum;
};

// The caller owns the private destination's temporary directory and keeps it
// alive through upload/retry. Only a complete read-only snapshot is returned;
// failed snapshots are removed here before any recursive upload can see them.
std::optional<StagedPayload> stagePayload(const QString &sourcePath, const QString &snapshotPath, BackupIssue *issue)
{
    const auto fail = [&](const QString &reason) -> std::optional<StagedPayload> {
        *issue = {sourcePath, QStringLiteral("reading"), reason};
        return std::nullopt;
    };
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("The source file could not be read: %1").arg(source.errorString()));
    }
    if (!QDir().mkpath(QFileInfo(snapshotPath).path())) {
        return fail(QStringLiteral("The backup payload folder could not be staged."));
    }

    QFile snapshot(snapshotPath);
    bool ready = false;
    const auto discardPartialSnapshot = qScopeGuard([&] {
        if (!ready) {
            snapshot.close();
            QFile::remove(snapshotPath);
        }
    });
    if (!snapshot.open(QIODevice::WriteOnly)) {
        return fail(QStringLiteral("The backup payload could not be staged: %1").arg(snapshot.errorString()));
    }
    QByteArray checksum;
    // Hash exactly the bytes copied into the snapshot, never reopen the source
    // pathname to calculate metadata or upload content that can change later.
    if (!copyAndHash(source, snapshot, &checksum)) {
        return fail(source.error() != QFileDevice::NoError
            ? QStringLiteral("The source file could not be read: %1").arg(source.errorString())
            : QStringLiteral("The backup payload could not be staged: %1").arg(snapshot.errorString()));
    }
    const qint64 size = snapshot.size();
    snapshot.close();
    source.close();
    if (!snapshot.setPermissions(QFileDevice::ReadOwner)) {
        return fail(QStringLiteral("The staged backup payload could not be made read-only."));
    }
    ready = true;
    return StagedPayload {snapshotPath, size, checksum};
}

}

BackupEngine::BackupEngine(QObject *parent)
    : QObject(parent)
{
}

bool BackupEngine::validateSelection(const QString &sourceDirectory, QString *error) const
{
    const QFileInfo source(sourceDirectory);

    if (!source.exists()) {
        if (error != nullptr) {
            *error = QStringLiteral("The selected folder does not exist.");
        }

        return false;
    }

    if (!source.isDir()) {
        if (error != nullptr) {
            *error = QStringLiteral("The selected path is not a folder.");
        }

        return false;
    }

    if (source.isSymLink()) {
        if (error != nullptr) {
            *error = QStringLiteral("Symbolic links are not valid backup roots.");
        }

        return false;
    }

    return true;
}

QStringList BackupEngine::selectableFiles(const QString &sourceDirectory) const
{
    return selectableFiles(QStringList {sourceDirectory}, {});
}

QStringList BackupEngine::selectableFiles(const QStringList &sourceDirectories, const QStringList &exclusions, const QStringList &inclusions) const
{
    return preview(sourceDirectories, exclusions, {}, inclusions).includedFiles;
}

QVariantMap BackupEngine::previewSelection(const QStringList &sourceDirectories, const QStringList &exclusions, const QStringList &inclusions) const
{
    const BackupPreview result = preview(sourceDirectories, exclusions, {}, inclusions);

    return {
        {QStringLiteral("included"), result.includedFiles},
        {QStringLiteral("excluded"), result.excludedFiles},
        {QStringLiteral("skipped"), result.skippedPaths},
        {QStringLiteral("missing"), result.missingPaths},
        {QStringLiteral("totalBytes"), result.totalBytes},
    };
}

BackupPreview BackupEngine::preview(const QStringList &sourceDirectories, const QStringList &exclusions, const std::function<bool()> &cancelled, const QStringList &inclusions) const
{
    return scan(sourceDirectories, exclusions, true, cancelled, inclusions);
}

BackupPreview BackupEngine::scan(const QStringList &sourceDirectories, const QStringList &exclusions, bool reportExcluded, const std::function<bool()> &cancelled, const QStringList &inclusions) const
{
    const ExclusionRules rules(exclusions);
    const InclusionRules includeRules(inclusions);
    BackupPreview result;
    QSet<QString> included;
    QStringList excluded;
    QStringList skipped;
    QStringList missing;
    const auto includeFile = [&](const QFileInfo &file) {
        const QString path = file.absoluteFilePath();
        if (included.contains(path)) return;
        included.insert(path);
        result.totalBytes += qMax(qint64(0), file.size());
    };

    for (const QString &sourceDirectory : sourceDirectories) {
        if (cancelled && cancelled()) return {};
        const QFileInfo source(sourceDirectory);
        if (!source.exists()) {
            missing.append(source.absoluteFilePath());
            continue;
        }
        if (isExcluded(source, rules)
            || ((!source.isDir() || source.isSymLink()) && !includeRules.matches(source, source))) {
            excluded.append(source.absoluteFilePath());
            continue;
        }
        if (source.isSymLink() || !source.isReadable()) {
            skipped.append(source.absoluteFilePath());
            continue;
        }
        if (source.isFile()) {
            includeFile(source);
            continue;
        }
        if (!source.isDir()) {
            skipped.append(source.absoluteFilePath());
            continue;
        }

        QStringList pending {source.absoluteFilePath()};
        while (!pending.isEmpty()) {
            if (cancelled && cancelled()) return {};
            QDirIterator iterator(pending.takeLast(), QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
            while (iterator.hasNext()) {
                if (cancelled && cancelled()) return {};
                iterator.next();
                const QFileInfo file = iterator.fileInfo();
                const QString path = file.absoluteFilePath();

                if (isExcluded(file, rules)) {
                    if (reportExcluded && file.isDir() && !file.isSymLink()) pending.append(path);
                    if (file.isFile() || file.isSymLink()) {
                        excluded.append(path);
                    }
                } else if ((!file.isDir() || file.isSymLink()) && !includeRules.matches(file, source)) {
                    if (reportExcluded) excluded.append(path);
                } else if (file.isSymLink()) {
                    skipped.append(path);
                } else if (file.isDir() && !file.isReadable()) {
                    skipped.append(path);
                } else if (file.isDir()) {
                    pending.append(path);
                } else if (file.isFile() && !file.isReadable()) {
                    skipped.append(path);
                } else if (file.isFile()) {
                    includeFile(file);
                }
            }
        }
    }

    if (cancelled && cancelled()) return {};
    // Relative rules can match through one overlapping root but not another.
    excluded.erase(std::remove_if(excluded.begin(), excluded.end(), [&](const QString &path) {
        return included.contains(path);
    }), excluded.end());
    excluded.removeDuplicates();
    skipped.removeDuplicates();
    missing.removeDuplicates();
    excluded.sort();
    skipped.sort();
    missing.sort();
    result.includedFiles = included.values();
    result.includedFiles.sort();
    result.excludedFiles = excluded;
    result.skippedPaths = skipped;
    result.missingPaths = missing;

    return result;
}

QString BackupEngine::previewError(const QString &sourceDirectory) const
{
    QString error;
    validateSelection(sourceDirectory, &error);

    return error;
}

bool BackupEngine::backup(const QString &sourceDirectory, const QString &remoteRoot, BackupProvider &provider, QString *manifestPath, QString *error) const
{
    return backup(QStringList {sourceDirectory}, remoteRoot, {}, provider, manifestPath, error);
}

bool BackupEngine::backup(const QStringList &sourceDirectories, const QString &remoteRoot, const QStringList &exclusions, BackupProvider &provider, QString *manifestPath, QString *error) const
{
    return backup(sourceDirectories, remoteRoot, exclusions, {}, provider, manifestPath, error);
}

bool BackupEngine::backup(const QStringList &sourceDirectories, const QString &remoteRoot, const QStringList &exclusions, const BackupCopyMetadata &metadata, BackupProvider &provider, QString *manifestPath, QString *error, const std::function<void(const BackupProgress &)> &reportProgress, BackupResult *result, const BackupOptions &options) const
{
    if (options.freshCopy) return backupBatches(sourceDirectories, remoteRoot, exclusions, metadata,
        provider, manifestPath, error, reportProgress, result, options);
    BackupResult outcome;
    outcome.reported = true;
    const auto finishResult = qScopeGuard([&] {
        if (result != nullptr) {
            *result = outcome;
        }
    });
    if (manifestPath != nullptr) {
        manifestPath->clear();
    }
    if (error != nullptr) {
        error->clear();
    }
    const QString normalizedRemoteRoot = QDir::cleanPath(remoteRoot);
    if (normalizedRemoteRoot.isEmpty() || normalizedRemoteRoot == QStringLiteral(".")
        || hasParentPathSegment(normalizedRemoteRoot)) {
        if (error != nullptr) {
            *error = QStringLiteral("The remote backup folder is invalid.");
        }

        return false;
    }

    const BackupPreview selection = scan(sourceDirectories, exclusions, false, {}, options.inclusions);
    for (const QString &path : selection.missingPaths) {
        outcome.issues.append({path, QStringLiteral("selection"), QStringLiteral("The source path does not exist.")});
    }
    for (const QString &path : selection.skippedPaths) {
        const QFileInfo file(path);
        outcome.issues.append({path, QStringLiteral("selection"), file.isSymLink()
            ? QStringLiteral("Symbolic links are not backed up.")
            : !file.isReadable() ? QStringLiteral("The source path is unreadable.")
                                 : QStringLiteral("The source path is not a regular file or folder.")});
    }
    if (selection.includedFiles.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("The selected folder contains no regular files.");
        }

        return false;
    }

    BackupProgress progress;
    progress.failedItems = outcome.issues.size();
    progress.phase = QStringLiteral("preparing");
    progress.totalFiles = selection.includedFiles.size();
    QHash<QString, qint64> plannedSizes;
    for (const QString &path : selection.includedFiles) {
        const qint64 size = qMax(qint64(0), QFileInfo(path).size());
        plannedSizes.insert(path, size);
        progress.totalBytes += size;
    }
    if (reportProgress) {
        reportProgress(progress);
    }

    BackupManifestDraft manifestDraft;
    manifestDraft.metadata = metadata;
    QVector<BackupEntry> pendingVerification;
    const auto recordVerified = [&](const BackupEntry &entry) {
        manifestDraft.verifiedEntries.append(entry);
        ++outcome.verifiedFiles;
        outcome.verifiedBytes += entry.size;
        progress.verifiedFiles = outcome.verifiedFiles;
        progress.verifiedBytes = outcome.verifiedBytes;
    };
    QStringList sourcePrefixes;
    QSet<QString> remotePaths;
    remotePaths.insert(QStringLiteral("manifest.json"));
    QString providerError;
    provider.beginBackupOperation();
    const auto endOperation = qScopeGuard([&] { provider.endBackupOperation(); });
    if (!provider.ensureDirectory(normalizedRemoteRoot, &providerError)) {
        if (error != nullptr) {
            *error = providerError.isEmpty() ? QStringLiteral("Unable to create the remote backup folder.") : providerError;
        }
        return false;
    }
    QSet<QString> ensuredDirectories {normalizedRemoteRoot};
    const bool uploadFolder = options.freshCopy && provider.supportsDirectoryUpload()
        && std::any_of(sourceDirectories.cbegin(), sourceDirectories.cend(), [](const QString &path) {
            return QFileInfo(path).isDir();
        });
    std::optional<QTemporaryDir> folderStaging;
    QString stagedFolder;
    if (uploadFolder) {
        folderStaging.emplace(QDir::temp().filePath(QStringLiteral("omacustos-backup-XXXXXX")));
        stagedFolder = folderStaging->filePath(QFileInfo(normalizedRemoteRoot).fileName());
        if (!folderStaging->isValid() || !QDir().mkpath(stagedFolder)) {
            if (error != nullptr) {
                *error = QStringLiteral("The backup staging folder could not be created.");
            }
            return false;
        }
    }
    for (const QString &sourceDirectory : sourceDirectories) {
        sourcePrefixes.append(remoteSegment(QFileInfo(sourceDirectory).fileName()));
    }
    QHash<QString, int> prefixCounts;
    for (QString &prefix : sourcePrefixes) {
        const int count = ++prefixCounts[prefix];
        if (count > 1) {
            prefix += QStringLiteral("-") + QString::number(count);
        }
    }

    for (const QString &sourcePath : selection.includedFiles) {
        bool stagedForFolder = false;
        // Count attempted files, including failures, without presenting those
        // failures as verified uploads. This measures remaining work only.
        const auto finishedFile = qScopeGuard([&] {
            // A staged file has not been uploaded yet. Count it only once the
            // folder transfer finishes, so preparation never reports 100% upload.
            if (stagedForFolder) return;
            ++progress.processedFiles;
            progress.processedBytes += plannedSizes.value(sourcePath);
            progress.currentFile.clear();
            progress.currentFileBytes = 0;
            if (reportProgress) {
                reportProgress(progress);
            }
        });
        int sourceIndex = 0;
        for (int index = 0; index < sourceDirectories.size(); ++index) {
            if (isWithinPath(sourcePath, cleanAbsolutePath(sourceDirectories.at(index)))) {
                sourceIndex = index;
                break;
            }
        }

        QString relativePath = QDir(cleanAbsolutePath(sourceDirectories.at(sourceIndex))).relativeFilePath(sourcePath);
        if (relativePath == QStringLiteral(".") || relativePath.startsWith(QStringLiteral("../"))) {
            relativePath = QFileInfo(sourcePath).fileName();
        }
        const QString mappedPath = sourceDirectories.size() > 1
            ? QDir(sourcePrefixes.at(sourceIndex)).filePath(relativePath)
            : relativePath;
        QString remoteMappedPath = mappedPath;
        int suffix = 1;
        while (remotePaths.contains(remoteMappedPath)) {
            remoteMappedPath = QStringLiteral("%1.%2").arg(mappedPath).arg(suffix++);
        }
        remotePaths.insert(remoteMappedPath);
        const QString remotePath = QDir(normalizedRemoteRoot).filePath(remoteMappedPath);
        manifestDraft.expectedItems.append(mappedPath);
        providerError.clear();

        const auto failFile = [&](const QString &phase, const QString &reason) {
            outcome.issues.append({sourcePath, phase, reason});
            manifestDraft.failedItems.append(mappedPath);
            ++progress.failedItems;
        };
        const auto reportPhase = [&](const QString &phase) {
            progress.currentFile = sourcePath;
            progress.currentFileBytes = plannedSizes.value(sourcePath);
            progress.phase = phase;
            if (reportProgress) {
                reportProgress(progress);
            }
        };
        reportPhase(uploadFolder ? QStringLiteral("staging") : QStringLiteral("reading"));

        // The orchestrator owns staging lifetime: per-file snapshots survive
        // upload/retry, while folder snapshots survive the whole-tree transfer.
        std::optional<QTemporaryDir> fileStaging;
        QString snapshotPath;
        if (uploadFolder) {
            snapshotPath = QDir(stagedFolder).filePath(remoteMappedPath);
        } else {
            fileStaging.emplace(QDir::temp().filePath(QStringLiteral("omacustos-payload-XXXXXX")));
            snapshotPath = fileStaging->filePath(QFileInfo(remotePath).fileName());
        }
        if (fileStaging && !fileStaging->isValid()) {
            failFile(QStringLiteral("reading"), QStringLiteral("The backup staging folder could not be created."));
            continue;
        }
        BackupIssue stagingIssue;
        const auto snapshot = stagePayload(sourcePath, snapshotPath, &stagingIssue);
        if (!snapshot) {
            failFile(stagingIssue.phase, stagingIssue.reason);
            continue;
        }
        if (uploadFolder) {
            pendingVerification.append({sourcePath, remotePath, snapshot->size, snapshot->checksum, mappedPath});
            stagedForFolder = true;
            continue;
        }
        RemoteFile remoteFile;
        if (!options.freshCopy) reportPhase(QStringLiteral("checking"));
        const bool alreadyVerified = !options.freshCopy && provider.inspect(remotePath, &remoteFile, &providerError)
            && PayloadMetadataPolicy::matchesForReuse(remoteFile, snapshot->size, snapshot->checksum);

        if (!alreadyVerified) {
            const QString parent = QFileInfo(remotePath).path();
            if (!ensuredDirectories.contains(parent) && !provider.ensureDirectory(parent, &providerError)) {
                failFile(QStringLiteral("preparing"), providerError.isEmpty()
                    ? QStringLiteral("The remote folder could not be created.")
                    : providerError);
                continue;
            }
            ensuredDirectories.insert(parent);
            reportPhase(QStringLiteral("uploading"));
            if (!provider.upload(snapshot->path, remotePath, &providerError)) {
                // An ensured directory may have been removed remotely. Recheck
                // it and retry the same immutable snapshot once, then verify.
                ensuredDirectories.remove(parent);
                if (!provider.ensureDirectory(parent, &providerError)
                    || !provider.upload(snapshot->path, remotePath, &providerError)) {
                    failFile(QStringLiteral("uploading"), providerError.isEmpty()
                        ? QStringLiteral("The file could not be uploaded.") : providerError);
                    continue;
                }
                ensuredDirectories.insert(parent);
            }
            if (!options.freshCopy) reportPhase(QStringLiteral("verifying"));
            if (!options.freshCopy && (!provider.inspect(remotePath, &remoteFile, &providerError)
                || !PayloadMetadataPolicy::matchesAfterTransfer(remoteFile, snapshot->size, snapshot->checksum))) {
                failFile(QStringLiteral("verifying"), providerError.isEmpty()
                    ? QStringLiteral("Remote verification failed.")
                    : providerError);
                continue;
            }
        }

        const BackupEntry entry {sourcePath, remotePath, snapshot->size, snapshot->checksum, mappedPath};
        if (options.freshCopy) {
            // Keep only metadata after a successful upload. The immutable staged
            // bytes remain available through upload retries, then are removed;
            // verification does not require another local payload copy.
            pendingVerification.append(entry);
        } else {
            recordVerified(entry);
        }
    }

    if (uploadFolder) {
        if (!pendingVerification.isEmpty()) {
            progress.phase = QStringLiteral("uploading-folder");
            progress.currentFile = sourceDirectories.size() == 1 ? sourceDirectories.first() : normalizedRemoteRoot;
            progress.currentFileBytes = 0;
            for (const BackupEntry &entry : pendingVerification) progress.currentFileBytes += entry.size;
            if (reportProgress) reportProgress(progress);
            providerError.clear();
            if (!provider.uploadDirectory(stagedFolder, normalizedRemoteRoot, &providerError)
                && (!provider.ensureDirectory(normalizedRemoteRoot, &providerError)
                    || !provider.uploadDirectory(stagedFolder, normalizedRemoteRoot, &providerError))) {
                const QString folderUploadError = providerError.isEmpty()
                    ? QStringLiteral("The backup folder could not be uploaded.") : providerError;
                // A failed recursive command can have transferred some files.
                // Keep those files restorable only after individual verification,
                // and never report the overall operation as successful.
                outcome.issues.append({progress.currentFile, QStringLiteral("uploading"), folderUploadError});
                ++progress.failedItems;
            }
            progress.processedFiles += pendingVerification.size();
            for (const BackupEntry &entry : pendingVerification) {
                progress.processedBytes += plannedSizes.value(entry.sourcePath);
            }
        }
        // No payload bytes are needed for verification. Free all staged data
        // immediately after the transfer/retry, including failed transfers.
        if (!folderStaging->remove()) {
            if (error != nullptr) {
                *error = QStringLiteral("Unable to remove the backup staging folder: %1").arg(folderStaging->path());
            }
            return false;
        }
    }

    progress.finalizing = true;
    RemoteMetadataCache directoryMetadata(provider);
    for (const BackupEntry &entry : pendingVerification) {
        progress.phase = QStringLiteral("verifying");
        progress.currentFile = entry.sourcePath;
        progress.currentFileBytes = entry.size;
        if (reportProgress) reportProgress(progress);
        RemoteFile remoteFile;
        QString verificationError;
        directoryMetadata.loadDirectory(QFileInfo(entry.remotePath).path(), &verificationError);
        const bool listed = directoryMetadata.lookup(entry.remotePath, &remoteFile);
        if ((!listed && !provider.inspect(entry.remotePath, &remoteFile, &verificationError))
            || !PayloadMetadataPolicy::matchesAfterTransfer(remoteFile, entry.size, entry.checksum)) {
            outcome.issues.append({entry.sourcePath, QStringLiteral("verifying"), verificationError.isEmpty()
                ? QStringLiteral("Remote verification failed.") : verificationError});
            manifestDraft.failedItems.append(entry.restorePath);
            ++progress.failedItems;
        } else {
            recordVerified(entry);
        }
    }
    progress.currentFile.clear();
    progress.currentFileBytes = 0;
    progress.phase = QStringLiteral("finalizing");
    if (reportProgress) {
        reportProgress(progress);
    }
    const QString manifestDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(
        QStringLiteral("omacustos-manifest-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!QDir().mkpath(manifestDirectory)
        || !QFile::setPermissions(manifestDirectory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to create the backup manifest folder.");
        }
        return false;
    }
    const QString path = QDir(manifestDirectory).filePath(QStringLiteral("manifest.json"));
    const bool incomplete = !outcome.issues.isEmpty();
    manifestDraft.failedItems.append(selection.missingPaths);
    manifestDraft.failedItems.append(selection.skippedPaths);
    manifestDraft.issues = outcome.issues;
    if (!BackupManifest::write(path, manifestDraft, error)) {
        return false;
    }

    const QString remoteManifestPath = QDir(normalizedRemoteRoot).filePath(QStringLiteral("manifest.json"));
    if (!provider.upload(path, remoteManifestPath, &providerError)) {
        if (error != nullptr) {
            *error = providerError.isEmpty() ? QStringLiteral("Unable to upload the backup manifest.") : providerError;
        }
        return false;
    }

    RemoteFile remoteManifest;
    if (!provider.inspect(remoteManifestPath, &remoteManifest, &providerError)
        || remoteManifest.size != QFileInfo(path).size()) {
        if (error != nullptr) {
            *error = providerError.isEmpty() ? QStringLiteral("Remote manifest verification failed.") : providerError;
        }
        return false;
    }

    outcome.manifestVerified = true;
    if (manifestPath != nullptr) {
        *manifestPath = path;
    }

    if (incomplete) {
        if (error != nullptr) {
            *error = (outcome.verifiedFiles > 0
                ? QStringLiteral("Backup incomplete: %1 files backed up · %2 items failed.")
                : QStringLiteral("Backup failed: %1 files backed up · %2 items failed."))
                .arg(outcome.verifiedFiles).arg(outcome.issues.size());
        }

        return false;
    }

    return true;
}

bool BackupEngine::restoreFile(const BackupEntry &entry, const QString &destinationDirectory, BackupProvider &provider, QString *error) const
{
    const BackupRestoreResult result = restoreFiles({{entry}, destinationDirectory, {}}, provider);
    if (error != nullptr) *error = result.error;
    return result.success;
}

BackupRestoreResult BackupEngine::restoreFiles(const BackupRestoreRequest &request, BackupProvider &provider,
    const std::function<void(int)> &reportProgress) const
{
    BackupRestoreResult result;
    QString issuePath = request.destinationDirectory;
    QString phase = QStringLiteral("Preparing restore");
    const auto checkpoint = [&] {
        if ((request.stopped && request.stopped()) || (request.checkpoint && !request.checkpoint())) {
            result.stopped = true;
            return false;
        }
        return true;
    };
    const auto failure = [&] {
        if (result.stopped || (request.stopped && request.stopped())) {
            result.stopped = true;
            result.error.clear();
        } else {
            if (result.error.isEmpty()) result.error = QStringLiteral("The selected restore file could not be restored.");
            result.issues.append({issuePath, phase, result.error});
        }
        return result;
    };
    if (request.entries.isEmpty()) {
        result.error = QStringLiteral("At least one restore file must be selected.");
        return failure();
    }
    QSet<QString> completedArchives;
    QSet<QString> identities;
    for (const auto &entry : request.entries) {
        const QString identity = QDir::cleanPath(effectiveRestorePath(entry));
        if (identities.contains(identity)) {
            result.error = QStringLiteral("The restore selection contains duplicate destinations.");
            issuePath = identity;
            return failure();
        }
        identities.insert(identity);
    }
    for (const BackupEntry &entry : request.entries) {
        // An archive group may already have placed the entire selection. Do not
        // acknowledge a late stop/pause while merely skipping its other members.
        if (!entry.archive.id.isEmpty() && completedArchives.contains(entry.archive.id)) continue;
        issuePath = effectiveRestorePath(entry);
        phase = QStringLiteral("Preparing restore");
        if (!checkpoint()) return failure();
        if (!entry.archive.id.isEmpty()) {
            QVector<BackupEntry> selected;
            for (const auto &candidate : request.entries) {
                if (candidate.archive.id != entry.archive.id) continue;
                if (candidate.archive.remotePath != entry.archive.remotePath
                    || candidate.remotePath != entry.archive.remotePath
                    || candidate.archive.size != entry.archive.size || candidate.archive.checksum != entry.archive.checksum
                    || candidate.archive.members != entry.archive.members || !BackupArchiveIO::safeMember(candidate.restorePath)
                    || (!request.copyPath.isEmpty()
                        && (!isWithinPath(candidate.remotePath, QDir::cleanPath(request.copyPath))
                            || candidate.remotePath != QDir::cleanPath(candidate.remotePath)
                            || hasParentPathSegment(candidate.remotePath)))) {
                    result.error = QStringLiteral("The restore selection contains ambiguous archive references.");
                    return failure();
                }
                selected.append(candidate);
            }
            QTemporaryDir workspace;
            const QString downloaded = workspace.filePath("archive.tar.gz");
            QVector<BackupEntry> payloads;
            issuePath = entry.archive.remotePath;
            phase = QStringLiteral("Downloading archive");
            if (!workspace.isValid() || !provider.download(entry.archive.remotePath, downloaded, &result.error)) return failure();
            if (!checkpoint()) return failure();
            phase = QStringLiteral("Verifying archive");
            if (!BackupArchiveIO::verify(downloaded, entry.archive.size, entry.archive.checksum)) {
                result.error = QStringLiteral("The downloaded archive failed size/SHA-256 verification.");
                return failure();
            }
            if (!checkpoint()) return failure();
            phase = QStringLiteral("Extracting archive");
            if (!BackupArchiveIO::extract(downloaded, selected, workspace.path(), &payloads, &result.error)) return failure();
            if (!checkpoint()) return failure();
            LocalProvider extracted(workspace.path());
            for (BackupEntry payload : payloads) {
                issuePath = effectiveRestorePath(payload);
                payload.remotePath = QFileInfo(payload.remotePath).fileName();
                if (!restoreEntry(payload, request.destinationDirectory, extracted, &result.error, checkpoint, &phase, request.stopped)) return failure();
                ++result.restoredCount;
                if (reportProgress) reportProgress(result.restoredCount);
            }
            completedArchives.insert(entry.archive.id);
            continue;
        }
        if (!restoreEntry(entry, request.destinationDirectory, provider, &result.error, checkpoint, &phase, request.stopped)) return failure();
        ++result.restoredCount;
        if (reportProgress) reportProgress(result.restoredCount);
    }
    result.success = true;
    return result;
}

bool BackupEngine::restoreEntry(const BackupEntry &entry, const QString &destinationDirectory, BackupProvider &provider,
    QString *error, const std::function<bool()> &checkpoint, QString *phase, const std::function<bool()> &stopped) const
{
    if (phase) *phase = QStringLiteral("Preparing destination");
    if (checkpoint && !checkpoint()) return false;
    if (error != nullptr) {
        error->clear();
    }
    const QString relativePath = effectiveRestorePath(entry);
    const QStringList relativeParts = relativePath.split('/', Qt::KeepEmptyParts);
    if (relativePath.isEmpty() || relativePath.startsWith('/')
        || std::any_of(relativeParts.cbegin(), relativeParts.cend(), [](const QString &part) {
            return part == QStringLiteral("..");
        })) {
        if (error != nullptr) {
            *error = QStringLiteral("The restore destination is outside the selected folder.");
        }

        return false;
    }
    const QString destination = QDir(destinationDirectory).filePath(relativePath);
    if (!QDir().mkpath(QFileInfo(destinationDirectory).absoluteFilePath())) {
        if (error != nullptr) {
            *error = QStringLiteral("The restore destination could not be created.");
        }
        return false;
    }
    const QString canonicalRoot = QFileInfo(destinationDirectory).canonicalFilePath();
    const QString destinationParent = QFileInfo(destination).absolutePath();
    if (canonicalRoot.isEmpty() || QFileInfo(destinationDirectory).isSymLink()
        || QFileInfo(destination).isSymLink()
        || !QDir().mkpath(destinationParent)) {
        if (error != nullptr) {
            *error = QStringLiteral("The restore destination is outside the selected folder.");
        }
        return false;
    }
    const QString canonicalParent = QFileInfo(destinationParent).canonicalFilePath();
    const QString canonicalDestination = QFileInfo(destination).exists()
        ? QFileInfo(destination).canonicalFilePath()
        : QDir(canonicalParent).filePath(QFileInfo(destination).fileName());

    if (canonicalRoot.isEmpty() || canonicalParent.isEmpty()
        || (canonicalParent != canonicalRoot && !canonicalParent.startsWith(canonicalRoot + QDir::separator()))
        || !canonicalDestination.startsWith(canonicalRoot + QDir::separator())) {
        if (error != nullptr) {
            *error = QStringLiteral("The restore destination is outside the selected folder.");
        }

        return false;
    }

    QTemporaryDir restoreStaging(QDir(destinationParent).filePath(QStringLiteral(".omacustos-restore-XXXXXX")));
    if (!restoreStaging.isValid()) {
        if (error != nullptr) {
            *error = QStringLiteral("The restore staging folder could not be created.");
        }
        return false;
    }
    const QString temporaryDestination = restoreStaging.filePath(QStringLiteral("payload"));
    if (phase) *phase = QStringLiteral("Downloading file");
    if (checkpoint && !checkpoint()) return false;
    if (!provider.download(entry.remotePath, temporaryDestination, error)) {
        QFile::remove(temporaryDestination);
        return false;
    }

    if (checkpoint && !checkpoint()) return false;
    if (phase) *phase = QStringLiteral("Verifying and placing file");
    QFile restoredFile(temporaryDestination);
    if (!restoredFile.open(QIODevice::ReadOnly)) {
        QFile::remove(temporaryDestination);
        if (error != nullptr) {
            *error = QStringLiteral("The restored file could not be opened for verification.");
        }

        return false;
    }

    // Recheck after the transfer: QSaveFile follows destination symlinks, and
    // neither a new symlink nor a changed parent may redirect verified content.
    const auto destinationUnchanged = [&] {
        return !QFileInfo(destinationDirectory).isSymLink() && !QFileInfo(destination).isSymLink()
            && QFileInfo(destinationDirectory).canonicalFilePath() == canonicalRoot
            && QFileInfo(destinationParent).canonicalFilePath() == canonicalParent;
    };
    if (!destinationUnchanged()) {
        if (error != nullptr) {
            *error = QStringLiteral("The restore destination is outside the selected folder.");
        }
        return false;
    }

    // Hash while writing the atomic replacement, so verification and placement
    // read the payload only once. Unverified bytes are never committed.
    QSaveFile replacement(destination);
    replacement.setDirectWriteFallback(false);
    const auto placementFailure = [&] {
        restoredFile.close();
        QFile::remove(temporaryDestination);
        if (error != nullptr) {
            *error = QStringLiteral("The restored file could not be placed in the destination folder.");
        }
        return false;
    };
    if (!replacement.open(QIODevice::WriteOnly)) return placementFailure();
    QByteArray checksum;
    if (!copyAndHash(restoredFile, replacement, &checksum, stopped)) return placementFailure();
    if (replacement.size() != entry.size
        || (!entry.checksum.isEmpty() && checksum != entry.checksum)) {
        if (error != nullptr) {
            *error = QStringLiteral("The restored file failed verification.");
        }
        return false;
    }
    // Copying a large file can take time; recheck immediately before commit too.
    if (checkpoint && !checkpoint()) return false;
    if (!destinationUnchanged()) {
        if (error != nullptr) {
            *error = QStringLiteral("The restore destination is outside the selected folder.");
        }
        return false;
    }
    if (!replacement.commit()) return placementFailure();
    restoredFile.close();
    QFile::remove(temporaryDestination);

    return true;
}

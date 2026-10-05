#include "recentbackupcopies.h"

#include "backupconfig.h"
#include "backupecleanup.h"
#include "backupmanifest.h"
#include "backuprunstore.h"

#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QTemporaryDir>
#include <QtConcurrentRun>
#include <algorithm>

namespace {

RecentCopyTarget inspectCopy(BackupProvider &provider, const BackupSet &set, const QString &computer,
    const QString &path)
{
    RecentCopyTarget target {set.id, set.name, path, {}, {}, {}};
    const QString root = QDir::cleanPath(set.remoteFolder(computer));
    if (QDir::cleanPath(path) != path || QFileInfo(path).path() != root) {
        target.error = QStringLiteral("The backup copy is outside its configured folder.");
        return target;
    }
    QTemporaryDir temporary;
    QString error;
    const QString manifestPath = temporary.filePath(QStringLiteral("manifest.json"));
    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    if (!temporary.isValid()
        || !provider.download(QDir(path).filePath(QStringLiteral("manifest.json")), manifestPath, &error)
        || !BackupManifest::load(manifestPath, &entries, &info, &error)) {
        target.error = error.isEmpty() ? QStringLiteral("Unable to identify the remote backup copy.") : error;
        return target;
    }
    if (info.version != 2 || info.application != QStringLiteral("omacustos") || info.setId != set.id
        || info.computerName != computer || info.copyId != QFileInfo(path).fileName()
        || !info.createdAt.isValid()
        || (info.status != QStringLiteral("complete") && info.status != QStringLiteral("incomplete"))) {
        target.error = QStringLiteral("The remote folder is not the expected OmaCustos backup copy.");
        return target;
    }
    target.copyId = info.copyId;
    target.createdAt = info.createdAt;
    return target;
}

RecentCopyTarget resolveCopy(BackupProvider &provider, const QString &configPath, const QString &computer,
    const QString &setId, const RecentCopyTarget &confirmed, bool browse)
{
    RecentCopyTarget result;
    result.setId = setId;
    const QString runPath = QDir(QFileInfo(configPath).absolutePath()).filePath(QStringLiteral("omacustos-backup-runs.json"));
    QLockFile managementGate(runPath + QStringLiteral(".management.lock"));
    QLockFile workerLock(configPath + QStringLiteral(".worker.lock"));
    QLockFile queueWorkerLock(runPath + QStringLiteral(".worker.lock"));
    workerLock.setStaleLockTime(0);
    queueWorkerLock.setStaleLockTime(0);
    managementGate.setStaleLockTime(0);
    if (!browse && (!managementGate.tryLock(0) || !workerLock.tryLock(0) || !queueWorkerLock.tryLock(0))) {
        result.error = QStringLiteral("Wait for the running backup to finish before managing its copies.");
        return result;
    }
    BackupConfig config;
    BackupRunStore runs(runPath);
    if (!BackupConfigStore(configPath).load(&config, &result.error) || !runs.load(&result.error)) {
        return result;
    }
    const auto set = std::find_if(config.sets.cbegin(), config.sets.cend(), [&setId](const BackupSet &candidate) {
        return candidate.id == setId;
    });
    if (set == config.sets.cend()) {
        result.error = QStringLiteral("The selected backup set no longer exists.");
        return result;
    }
    runs.ensureSet(setId);
    BackupRunRecord *record = runs.find(setId);
    if (record->status == QStringLiteral("copy_deleted")) {
        result.error = QStringLiteral("This recent backup copy has already been deleted.");
        return result;
    }
    if (record->status == QStringLiteral("pending") || record->status == QStringLiteral("running")
        || record->status == QStringLiteral("retrying") || record->status == QStringLiteral("waiting")) {
        result.error = QStringLiteral("Wait for the queued backup to finish before managing its copies.");
        return result;
    }
    if (!confirmed.path.isEmpty()) {
        if (record->remoteCopyPath != confirmed.path) {
            result.error = QStringLiteral("The recent backup copy changed. Choose Delete again to review it.");
            return result;
        }
        result = inspectCopy(provider, *set, computer, confirmed.path);
        if (!result.error.isEmpty()) {
            return result;
        }
        if (result.copyId != confirmed.copyId || result.createdAt != confirmed.createdAt) {
            result.error = QStringLiteral("The remote backup copy changed since confirmation.");
            return result;
        }
        CleanupStore cleanup(QDir(QFileInfo(configPath).absolutePath()).filePath(QStringLiteral("omacustos-backup-cleanup.json")));
        if (!cleanup.load(&result.error)) {
            return result;
        }
        if (!provider.trash(result.path, &result.error)) {
            if (result.error.isEmpty()) {
                result.error = QStringLiteral("Unable to delete the remote backup copy.");
            }
            return result;
        }
        if (cleanup.forgetTarget(setId, result.path, &result.error)) {
            runs.markCopyDeleted(setId, result.path, &result.error);
        }
        return result;
    }
    if (!record->remoteCopyPath.isEmpty()) {
        if (browse) {
            // Navigation needs only the recorded folder; manifest identity is
            // checked for deletion and restore, not for opening the web app.
            result.name = set->name;
            result.path = record->remoteCopyPath;
            if (QDir::cleanPath(result.path) != result.path
                || QFileInfo(result.path).path() != QDir::cleanPath(set->remoteFolder(computer))) {
                result.error = QStringLiteral("The backup copy is outside its configured folder.");
            }
            return result;
        }
        return inspectCopy(provider, *set, computer, record->remoteCopyPath);
    }

    // Older run records have no exact path. Identify their newest copy by its manifest once.
    if (browse && (!managementGate.tryLock(0) || !workerLock.tryLock(0) || !queueWorkerLock.tryLock(0))) {
        result.error = QStringLiteral("Wait for the running backup or queue update to finish before discovering older copies.");
        return result;
    }
    const QString root = QDir::cleanPath(set->remoteFolder(computer));
    QVector<RemoteItem> folders;
    if (!provider.list(root, &folders, &result.error)) {
        return result;
    }
    for (const RemoteItem &folder : folders) {
        if (!folder.directory || QFileInfo(folder.path).path() != root) {
            continue;
        }
        const RecentCopyTarget candidate = inspectCopy(provider, *set, computer, folder.path);
        if (candidate.error.isEmpty() && (!result.createdAt.isValid() || candidate.createdAt > result.createdAt)) {
            result = candidate;
        }
    }
    if (!result.createdAt.isValid()) {
        result.error = QStringLiteral("No identifiable OmaCustos backup copy was found for this backup set.");
        return result;
    }
    const BackupRunRecord expected = *record;
    runs.rememberCopyPath(expected, result.path, &result.error);
    return result;
}

}

RecentBackupCopies::RecentBackupCopies(BackupProvider &provider, QString configPath, QString computerName, QObject *parent)
    : QObject(parent)
    , provider(provider)
    , configPath(std::move(configPath))
    , computerName(std::move(computerName))
{
    connect(&watcher, &QFutureWatcher<RecentCopyTarget>::finished, this, [this] {
        const RecentCopyTarget result = watcher.result();
        resolving = false;
        emit busyChanged();
        if (!result.error.isEmpty()) {
            pending = {};
            emit failed(result.error);
        } else if (operation == Operation::PrepareDelete) {
            pending = result;
            emit deleteConfirmationReady(result.name, result.path);
        } else if (operation == Operation::Browse) {
            emit folderResolved(result.path);
        } else {
            pending = {};
            emit copyDeleted(result.setId);
            emit statusChanged(QStringLiteral("Backup copy moved to Proton Drive Trash."));
        }
    });
}

RecentBackupCopies::~RecentBackupCopies()
{
    watcher.waitForFinished();
}

bool RecentBackupCopies::busy() const
{
    return resolving;
}

QString RecentBackupCopies::deletingSetId() const
{
    return resolving && operation == Operation::Delete ? pending.setId : QString {};
}

void RecentBackupCopies::openCopy(const QString &setId)
{
    start(Operation::Browse, setId);
}

void RecentBackupCopies::requestDelete(const QString &setId)
{
    start(Operation::PrepareDelete, setId);
}

void RecentBackupCopies::cancelDelete()
{
    if (!resolving) {
        pending = {};
    }
}

void RecentBackupCopies::confirmDelete()
{
    if (!pending.path.isEmpty()) {
        start(Operation::Delete, pending.setId);
    }
}

void RecentBackupCopies::start(Operation nextOperation, const QString &setId)
{
    if (resolving) {
        return;
    }
    if (nextOperation != Operation::Delete) {
        pending = {};
    }
    const RecentCopyTarget confirmed = nextOperation == Operation::Delete ? pending : RecentCopyTarget {};
    const bool browse = nextOperation == Operation::Browse;
    operation = nextOperation;
    resolving = true;
    emit busyChanged();
    watcher.setFuture(QtConcurrent::run([this, setId, confirmed, browse] {
        return resolveCopy(provider, configPath, computerName, setId, confirmed, browse);
    }));
}

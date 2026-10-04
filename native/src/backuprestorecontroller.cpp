#include "backuprestorecontroller.h"

#include "backupmanifest.h"

#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QtConcurrentRun>

BackupRestoreController::BackupRestoreController(BackupEngine &engine, BackupProvider *provider, QObject *parent)
    : QObject(parent)
    , engine(engine)
    , provider(provider)
{
    operations.setMaxThreadCount(1);
    // Eligibility depends on verification and on both browse/restore busy states.
    connect(this, &BackupRestoreController::busyChanged,
        this, &BackupRestoreController::restoreEligibilityChanged);
    connect(&watcher, &QFutureWatcher<BrowseResult>::finished, this, [this] {
        const BrowseResult result = watcher.result();
        browseInFlight = false;
        if (activeBrowse.generation != browseGeneration) {
            if (loading) startBrowse();
            return;
        }
        if (activeBrowse.listing) {
            if (result.success) {
                QHash<QString, RemoteCopy> previous;
                for (const auto &copy : remoteCopies) previous.insert(copy.rootPath, copy);
                remoteCopies = result.copies;
                bool retainedSnapshot = false;
                for (auto &copy : remoteCopies) {
                    const auto old = previous.constFind(copy.rootPath);
                    if (old != previous.cend() && (!old->entries.isEmpty()
                        || !old->unavailableItems.isEmpty() || !old->failedItems.isEmpty())) {
                        copy = *old;
                        retainedSnapshot = true;
                    }
                }
                selectedCopyIndex = -1;
                if (!activeBrowse.selectedPath.isEmpty()) {
                    for (int index = 0; index < remoteCopies.size(); ++index) {
                        if (remoteCopies.at(index).rootPath == activeBrowse.selectedPath) {
                            selectedCopyIndex = index;
                            break;
                        }
                    }
                }
                if (selectedCopyIndex < 0) {
                    manifestEntries.clear();
                }
                // Listing copies does not reverify the retained file list.
                setCachedData(retainedSnapshot || !manifestEntries.isEmpty());
            }
        } else if (result.success) {
            remoteCopies[selectedCopyIndex] = result.copy;
            manifestEntries = result.copy.entries;
            setCachedData(false);
            if (!verified) {
                verified = true;
                emit restoreEligibilityChanged();
            }
        }
        loading = false;
        refreshError = result.success ? QString() : result.error;
        emit copiesChanged();
        emit currentCopyIndexChanged();
        publishEntries();
        emit busyChanged();
        if (!result.success) {
            setCachedData(!remoteCopies.isEmpty() || !manifestEntries.isEmpty());
            if (!activeBrowse.listing && verified) {
                verified = false;
                emit restoreEligibilityChanged();
            }
            emit failed(result.error);
        } else if (activeBrowse.listing) {
            emit statusChanged(QStringLiteral("%1 backup copies found. Select one to load and verify its files.").arg(remoteCopies.size()));
        } else {
            const QString status = QStringLiteral("%1 verified files available; %2 unavailable or failed.")
                .arg(manifestEntries.size()).arg(unavailableEntries().size());
            emit statusChanged(result.error.isEmpty() ? status : status + QStringLiteral(" ") + result.error);
        }
    });
    connect(&restoreWatcher, &QFutureWatcher<RestoreResult>::finished, this, [this] {
        const RestoreResult result = restoreWatcher.result();
        const QString completedFolder = transferBackupFolder;
        const QString completedSet = transferBackupId;
        const QString completedCopy = transferCopyPath;
        restoring = false;
        transferred = result.restoredCount;
        emit restoreProgressChanged();
        emit busyChanged();
        if (!result.success) {
            emit failed(result.error);
            return;
        }

        emit statusChanged(result.restoredCount == 1
            ? QStringLiteral("File restored successfully.")
            : QStringLiteral("%1 files restored successfully.").arg(result.restoredCount));
        emit restoreCompleted();
        emit restoreCompletedForContext(completedFolder, completedSet, completedCopy);
    });
}

BackupRestoreController::~BackupRestoreController()
{
    if (browseCancelled) browseCancelled->store(true);
    watcher.waitForFinished();
    restoreWatcher.waitForFinished();
}

bool BackupRestoreController::busy() const
{
    return loading || restoring;
}

QString BackupRestoreController::loadingMessage() const
{
    if (!loading) return restoring ? QStringLiteral("Restoring files…") : QString();
    const QString message = discovering ? QStringLiteral("Loading backup copies…")
                                        : QStringLiteral("Loading and verifying files…");
    return restoring ? message + QStringLiteral(" Waiting for the active restore.") : message;
}

bool BackupRestoreController::restoreEligible() const
{
    return verified && !busy();
}

QString BackupRestoreController::currentCopyPath() const
{
    return selectedCopyIndex >= 0 && selectedCopyIndex < remoteCopies.size()
        ? remoteCopies.at(selectedCopyIndex).rootPath : QString();
}

QString BackupRestoreController::restoreProgress() const
{
    return transferTotal > 0 ? QStringLiteral("%1 of %2 files restored").arg(transferred).arg(transferTotal) : QString();
}

double BackupRestoreController::restoreProgressFraction() const
{
    return transferTotal > 0 ? double(transferred) / transferTotal : 0;
}

bool BackupRestoreController::showingCachedData() const
{
    return cachedData;
}

void BackupRestoreController::setCachedData(bool cached)
{
    if (cachedData == cached) {
        return;
    }
    cachedData = cached;
    emit cachedDataChanged();
}

int BackupRestoreController::currentCopyIndex() const
{
    return filteredCopyIndexes().indexOf(selectedCopyIndex);
}

QStringList BackupRestoreController::entries() const
{
    return entryPaths;
}

void BackupRestoreController::publishEntries()
{
    entryPaths.clear();
    entryPaths.reserve(manifestEntries.size());
    for (const BackupEntry &entry : manifestEntries) {
        entryPaths.append(entry.sourcePath);
    }
    emit entriesChanged();
}

QString BackupRestoreController::defaultDestination() const
{
    return QDir::home().filePath(QStringLiteral("OmaCustos restore"));
}

QStringList BackupRestoreController::copies() const
{
    QStringList result;
    for (const int index : filteredCopyIndexes()) {
        const RemoteCopy &copy = remoteCopies.at(index);
        result.append(QStringLiteral("%1 / %2 / %3%4 (%5)%6")
            .arg(copy.computerName, copy.setName,
                copy.copyId, copy.createdAt.isValid()
                    ? QStringLiteral(" / ") + copy.createdAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                    : QString(), copy.status,
                copy.unavailableItems.isEmpty() && copy.failedItems.isEmpty()
                    ? QString()
                    : QStringLiteral(" - some items unavailable")));
    }
    return result;
}

QString BackupRestoreController::copySearch() const
{
    return searchText;
}

void BackupRestoreController::setCopySearch(const QString &search)
{
    if (searchText == search) {
        return;
    }
    searchText = search;
    emit copiesChanged();
    emit currentCopyIndexChanged();
}

QVector<int> BackupRestoreController::filteredCopyIndexes() const
{
    QVector<int> result;
    const QString query = searchText.trimmed().toLower();
    for (int index = 0; index < remoteCopies.size(); ++index) {
        const RemoteCopy &copy = remoteCopies.at(index);
        const QString haystack = QStringLiteral("%1 %2 %3 %4 %5")
            .arg(copy.computerName, copy.setName, copy.copyId, copy.status, copy.createdAt.toString());
        // Preserve the chosen copy and its selection while filtering, including
        // when verification changes its name/status.
        if (index == selectedCopyIndex || query.isEmpty() || haystack.toLower().contains(query)) {
            result.append(index);
        }
    }
    return result;
}

QStringList BackupRestoreController::unavailableEntries() const
{
    if (selectedCopyIndex < 0 || selectedCopyIndex >= remoteCopies.size()) {
        return {};
    }
    QStringList unavailable = remoteCopies.at(selectedCopyIndex).unavailableItems;
    unavailable.append(remoteCopies.at(selectedCopyIndex).failedItems);
    unavailable.removeDuplicates();
    return unavailable;
}

void BackupRestoreController::loadManifest(const QString &path)
{
    if (busy()) {
        return;
    }
    QVector<BackupEntry> loadedEntries;
    QString error;
    if (!BackupManifest::load(path, &loadedEntries, &error)) {
        if (verified) {
            verified = false;
            emit restoreEligibilityChanged();
        }
        manifestEntries.clear();
        remoteCopies.clear();
        setCachedData(false);
        selectedCopyIndex = -1;
        emit copiesChanged();
        emit currentCopyIndexChanged();
        publishEntries();
        emit failed(error);

        return;
    }

    manifestEntries = std::move(loadedEntries);
    setCachedData(false);
    if (!verified) {
        verified = true;
        emit restoreEligibilityChanged();
    }
    remoteCopies.clear();
    selectedCopyIndex = -1;
    emit copiesChanged();
    emit currentCopyIndexChanged();
    publishEntries();
    emit statusChanged(QStringLiteral("%1 files available for restore.").arg(manifestEntries.size()));
}

void BackupRestoreController::discover(const QString &backupFolder, const QString &setId)
{
    if (provider == nullptr) {
        emit failed(QStringLiteral("No backup provider is configured."));
        return;
    }

    if (browseCancelled) browseCancelled->store(true);
    const bool sameContext = activeBackupFolder == backupFolder && expectedSetId == setId;
    if (!sameContext) {
        if (!activeBackupFolder.isEmpty()) {
            snapshots.insert(activeBackupFolder + QChar(0) + expectedSetId,
                new Snapshot {remoteCopies, manifestEntries, selectedCopyIndex, searchText});
        }
        const Snapshot *saved = snapshots.object(backupFolder + QChar(0) + setId);
        remoteCopies = saved ? saved->copies : QVector<RemoteCopy>();
        manifestEntries = saved ? saved->entries : QVector<BackupEntry>();
        selectedCopyIndex = saved ? saved->selectedIndex : -1;
        searchText = saved ? saved->search : QString();
    }
    setCachedData(!remoteCopies.isEmpty() || !manifestEntries.isEmpty());
    if (verified) {
        verified = false;
        emit restoreEligibilityChanged();
    }
    expectedSetId = setId;
    activeBackupFolder = backupFolder;
    discovering = true;
    loading = true;
    refreshError.clear();
    pendingBrowse = {++browseGeneration, true, backupFolder, setId, {}, currentCopyPath()};
    emit busyChanged();
    emit copiesChanged();
    emit currentCopyIndexChanged();
    publishEntries();
    if (!browseInFlight) startBrowse();
}

void BackupRestoreController::selectCopy(int index)
{
    if (provider == nullptr) {
        return;
    }
    if (browseCancelled) browseCancelled->store(true);
    const QVector<int> indexes = filteredCopyIndexes();
    const int actualIndex = index >= 0 && index < indexes.size() ? indexes.at(index) : -1;
    const int previousIndex = selectedCopyIndex;
    selectedCopyIndex = actualIndex;
    if (actualIndex != previousIndex) {
        manifestEntries = actualIndex >= 0 ? remoteCopies.at(actualIndex).entries : QVector<BackupEntry>();
    }
    if (verified) {
        verified = false;
        emit restoreEligibilityChanged();
    }
    setCachedData(!manifestEntries.isEmpty());
    if (actualIndex >= 0) {
        // Reverify on each selection: remote files may have changed since the last visit.
        discovering = false;
        loading = true;
    }
    ++browseGeneration;
    refreshError.clear();
    loading = actualIndex >= 0;
    pendingBrowse = {browseGeneration, false, activeBackupFolder, expectedSetId, currentCopyPath(), {}};
    emit busyChanged();
    emit currentCopyIndexChanged();
    publishEntries();
    if (actualIndex < 0) {
        return;
    }
    if (!browseInFlight) startBrowse();
}

void BackupRestoreController::startBrowse()
{
    activeBrowse = pendingBrowse;
    browseInFlight = true;
    const BrowseRequest request = activeBrowse;
    browseCancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = browseCancelled;
    BackupProvider *workerProvider = provider;
    watcher.setFuture(QtConcurrent::run(&operations, [workerProvider, request, cancelled] {
        BrowseResult result;
        if (cancelled->load()) return result;
        if (request.listing) {
            result.success = BackupCatalog::listCopies(*workerProvider, request.folder, &result.copies, &result.error);
        } else {
            result.success = BackupCatalog::verifyCopy(*workerProvider, request.copyPath, request.setId,
                &result.copy, &result.error, [cancelled] { return cancelled->load(); });
        }
        return result;
    }));
}

void BackupRestoreController::restore(int index, const QString &destinationDirectory)
{
    if (busy()) {
        return;
    }
    if (destinationDirectory.trimmed().isEmpty()) {
        emit failed(QStringLiteral("A restore destination folder is required."));

        return;
    }

    if (provider == nullptr) {
        emit failed(QStringLiteral("No backup provider is configured."));

        return;
    }

    if (!restoreEligible()) {
        emit failed(QStringLiteral("The selected backup copy is not currently verified."));
        return;
    }

    if (index < 0 || index >= manifestEntries.size()) {
        emit failed(QStringLiteral("The selected restore file is invalid."));

        return;
    }

    startRestore({manifestEntries.at(index)}, destinationDirectory);
}

void BackupRestoreController::restoreSelected(const QVariantList &indexes, const QString &destinationDirectory)
{
    if (busy()) {
        return;
    }
    if (indexes.isEmpty()) {
        emit failed(QStringLiteral("At least one restore file must be selected."));
        return;
    }

    if (destinationDirectory.trimmed().isEmpty()) {
        emit failed(QStringLiteral("A restore destination folder is required."));
        return;
    }

    if (provider == nullptr) {
        emit failed(QStringLiteral("No backup provider is configured."));
        return;
    }

    if (!restoreEligible()) {
        emit failed(QStringLiteral("The selected backup copy is not currently verified."));
        return;
    }

    QVector<BackupEntry> entries;
    entries.reserve(indexes.size());
    for (const QVariant &value : indexes) {
        const int index = value.toInt();
        if (index < 0 || index >= manifestEntries.size()) {
            emit failed(QStringLiteral("The selected restore file is invalid."));
            return;
        }
        entries.append(manifestEntries.at(index));
    }

    startRestore(entries, destinationDirectory);
}

void BackupRestoreController::startRestore(const QVector<BackupEntry> &entries, const QString &destination)
{
    BackupEngine *enginePointer = &engine;
    BackupProvider *providerPointer = provider;
    restoring = true;
    transferBackupFolder = activeBackupFolder;
    transferBackupId = expectedSetId;
    transferCopyPath = currentCopyPath();
    transferTotal = entries.size();
    transferred = 0;
    emit restoreProgressChanged();
    emit busyChanged();
    restoreWatcher.setFuture(QtConcurrent::run(&operations, [this, enginePointer, providerPointer, entries, destination] {
        RestoreResult result;
        result.success = true;
        QElapsedTimer progressTimer;
        progressTimer.start();
        for (const BackupEntry &entry : entries) {
            if (!enginePointer->restoreFile(entry, destination, *providerPointer, &result.error)) {
                result.success = false;
                if (result.error.isEmpty()) {
                    result.error = QStringLiteral("The selected restore file could not be restored.");
                }
                return result;
            }
            ++result.restoredCount;
            const int count = result.restoredCount;
            // Bound GUI notification traffic while still reporting each slow
            // file's completion, and always publish the first and last file.
            if (count == 1 || progressTimer.elapsed() >= 100 || count == entries.size()) {
                progressTimer.restart();
                QMetaObject::invokeMethod(this, [this, count] {
                    transferred = count;
                    emit restoreProgressChanged();
                }, Qt::QueuedConnection);
            }
        }
        return result;
    }));
}

void BackupRestoreController::restoreFolder(const QString &folder, const QString &destinationDirectory)
{
    const QString prefix = QDir::cleanPath(folder).trimmed();
    QVariantList indexes;
    for (int index = 0; index < manifestEntries.size(); ++index) {
        const QString path = manifestEntries.at(index).restorePath;
        if (path == prefix || path.startsWith(prefix + QDir::separator())) {
            indexes.append(index);
        }
    }
    restoreSelected(indexes, destinationDirectory);
}

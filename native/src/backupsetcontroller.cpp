#include "backupsetcontroller.h"
#include "backupstaging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSysInfo>
#include <QLocale>
#include <QSaveFile>
#include <QUuid>
#include <QtConcurrentRun>

#include <algorithm>

namespace {

bool readState(const QString &path, QByteArray *contents, QString *error)
{
    QFile file(path);
    if (!file.exists()) {
        *contents = QByteArray(1, '\0');
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QObject::tr("Unable to read local state: %1").arg(file.errorString());
        return false;
    }
    *contents = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        *error = QObject::tr("Unable to read local state: %1").arg(file.errorString());
        return false;
    }
    contents->prepend('\1');
    return true;
}

bool isRunActive(const QString &status)
{
    return status == QStringLiteral("running");
}

QString statusLabel(const QString &status)
{
    if (status == QStringLiteral("success")) return QObject::tr("Successful");
    if (status == QStringLiteral("incomplete")) return QObject::tr("Incomplete");
    if (status == QStringLiteral("failed")) return QObject::tr("Failed");
    if (status == QStringLiteral("running")) return QObject::tr("Running");
    if (status == QStringLiteral("pending")) return QObject::tr("Queued");
    if (status == QStringLiteral("retrying")) return QObject::tr("Retrying");
    if (status == QStringLiteral("waiting")) return QObject::tr("Waiting");
    if (status == QStringLiteral("authentication_required")) return QObject::tr("Sign-in required");
    if (status == QStringLiteral("paused")) return QObject::tr("Paused");
    if (status == QStringLiteral("cancelled")) return QObject::tr("Cancelled (partial copy)");
    return status;
}

QString phaseLabel(const QString &phase)
{
    if (phase == QStringLiteral("reading")) return QObject::tr("Reading");
    if (phase == QStringLiteral("staging")) return QObject::tr("Preparing backup folder");
    if (phase == QStringLiteral("checking")) return QObject::tr("Checking remote file");
    if (phase == QStringLiteral("uploading")) return QObject::tr("Uploading");
    if (phase == QStringLiteral("uploading-folder")) return QObject::tr("Uploading folder");
    if (phase == QStringLiteral("verifying")) return QObject::tr("Verifying");
    if (phase == QStringLiteral("selection")) return QObject::tr("Selecting sources");
    if (phase == QStringLiteral("finalizing")) return QObject::tr("Finalizing backup");
    if (phase == QStringLiteral("scanning")) return QObject::tr("Scanning sources");
    if (phase == QStringLiteral("checkpointing")) return QObject::tr("Saving verified progress");
    return QObject::tr("Preparing backup");
}

QString resultSummary(const BackupResult &result)
{
    return QObject::tr("%1 files backed up · %2 items failed")
        .arg(result.verifiedFiles).arg(result.issues.size());
}

BackupSet newSet(int number)
{
    return {
        QUuid::createUuid().toString(QUuid::WithoutBraces),
        QStringLiteral("Backup %1").arg(number),
        QStringLiteral("/my-files/backups"),
        {},
        {},
    };
}

QStringList draftPaths(const QVariant &value)
{
    QStringList paths;
    for (const QString &entry : value.toStringList()) {
        const QString path = entry.trimmed();
        if (!path.isEmpty()) paths.append(path);
    }
    return paths;
}

}

BackupSetController::BackupSetController(BackupEngine &engine, QString configPath, QObject *parent)
    : QObject(parent)
    , engine(engine)
    , store(configPath)
    , runStore(QDir(QFileInfo(configPath).absolutePath()).filePath(QStringLiteral("omacustos-backup-runs.json")))
    , cleanupStore(QDir(QFileInfo(configPath).absolutePath()).filePath(QStringLiteral("omacustos-backup-cleanup.json")))
{
    connect(&previewWatcher, &QFutureWatcher<BackupPreview>::finished, this, [this] {
        scanInFlight = false;
        if (activePreviewGeneration != previewGeneration) {
            if (previewWorking) startPreview();
            return;
        }
        previewResult = previewWatcher.result();
        hasPreview = true;
        previewWorking = false;
        emit previewChanged();
        emit previewBusyChanged();
        emit statusChanged(QString());
    });
    connect(&stateTimer, &QTimer::timeout, this, &BackupSetController::refreshRunState);
    stateTimer.start(5000);
    runStore.load(nullptr, &runContents);
    cleanupStore.load(nullptr, &cleanupContents);
    QString error;
    if (store.load(&config, &error)) {
        selectedIndex = config.sets.isEmpty() ? -1 : 0;
    } else if (QFileInfo::exists(store.filePath())) {
        emit failed(error);
    } else {
        config.protonBinary = qEnvironmentVariable("OMACUSTOS_PROTON_BIN", QStringLiteral("proton-drive"));
        selectedIndex = -1;
    }
    connect(this, &BackupSetController::setsChanged, this, &BackupSetController::updateDashboard);
    connect(this, &BackupSetController::setsChanged, this, &BackupSetController::runDetailsChanged);
    connect(this, &BackupSetController::currentSetChanged, this, &BackupSetController::updateDashboard);
    connect(this, &BackupSetController::currentSetChanged, this, &BackupSetController::runStateChanged);
    updateDashboard();
    stateTimer.setInterval(cachedRunningSetIds.isEmpty() ? 5000 : 1000);
}

BackupSetController::~BackupSetController()
{
    if (previewCancelled) previewCancelled->store(true);
    previewWatcher.waitForFinished();
}

QStringList BackupSetController::setNames() const
{
    QStringList names;
    for (const BackupSet &set : config.sets) {
        names.append(set.name);
    }

    return names;
}

QStringList BackupSetController::setIds() const
{
    QStringList ids;
    for (const BackupSet &set : config.sets) {
        ids.append(set.id);
    }
    return ids;
}

QString BackupSetController::currentId() const
{
    const BackupSet *set = currentSet();
    return set == nullptr ? QString() : set->id;
}

int BackupSetController::currentIndex() const
{
    return selectedIndex;
}

void BackupSetController::setCurrentIndex(int index)
{
    const int nextIndex = config.sets.isEmpty() ? -1 : qBound(0, index, config.sets.size() - 1);
    if (nextIndex == selectedIndex) return;
    if (config.sets.isEmpty()) {
        selectedIndex = -1;
    } else {
        selectedIndex = qBound(0, index, config.sets.size() - 1);
    }

    emit currentIndexChanged();
    emit currentSetChanged();
    clearPreview();
}

QString BackupSetController::currentName() const
{
    const BackupSet *set = currentSet();
    return set == nullptr ? QString() : set->name;
}

void BackupSetController::setCurrentName(const QString &name)
{
    if (BackupSet *set = currentSet()) {
        if (set->name == name) return;
        set->name = name;
        emit setsChanged();
        emit currentSetChanged();
    }
}

QString BackupSetController::currentRemoteRoot() const
{
    const BackupSet *set = currentSet();
    return set == nullptr ? QString() : set->remoteRoot;
}

void BackupSetController::setCurrentRemoteRoot(const QString &remoteRoot)
{
    if (BackupSet *set = currentSet()) {
        set->remoteRoot = remoteRoot;
        emit currentSetChanged();
    }
}

QStringList BackupSetController::currentSources() const
{
    const BackupSet *set = currentSet();
    return set == nullptr ? QStringList() : set->sourceDirectories;
}

void BackupSetController::setCurrentSources(const QStringList &sources)
{
    if (BackupSet *set = currentSet()) {
        if (set->sourceDirectories == sources) return;
        set->sourceDirectories = sources;
        clearPreview();
        emit currentSetChanged();
    }
}

QStringList BackupSetController::currentExclusions() const
{
    const BackupSet *set = currentSet();
    return set == nullptr ? QStringList() : set->exclusions;
}

void BackupSetController::setCurrentExclusions(const QStringList &exclusions)
{
    if (BackupSet *set = currentSet()) {
        if (set->exclusions == exclusions) return;
        set->exclusions = exclusions;
        clearPreview();
        emit currentSetChanged();
    }
}

QString BackupSetController::currentScheduleFrequency() const
{
    const BackupSet *set = currentSet();
    return set == nullptr ? QStringLiteral("disabled") : set->schedule.frequency;
}

void BackupSetController::setCurrentScheduleFrequency(const QString &frequency)
{
    if (BackupSet *set = currentSet()) {
        set->schedule.frequency = frequency;
        emit currentSetChanged();
    }
}

int BackupSetController::currentScheduleHour() const
{
    const BackupSet *set = currentSet();
    return set == nullptr ? 2 : set->schedule.hour;
}

void BackupSetController::setCurrentScheduleHour(int hour)
{
    if (BackupSet *set = currentSet()) {
        set->schedule.hour = qBound(0, hour, 23);
        emit currentSetChanged();
    }
}

int BackupSetController::currentScheduleMinute() const
{
    const BackupSet *set = currentSet();
    return set == nullptr ? 0 : set->schedule.minute;
}

void BackupSetController::setCurrentScheduleMinute(int minute)
{
    if (BackupSet *set = currentSet()) {
        set->schedule.minute = qBound(0, minute, 59);
        emit currentSetChanged();
    }
}

int BackupSetController::currentScheduleWeekday() const
{
    const BackupSet *set = currentSet();
    return set == nullptr ? 1 : set->schedule.weekday;
}

void BackupSetController::setCurrentScheduleWeekday(int weekday)
{
    if (BackupSet *set = currentSet()) {
        set->schedule.weekday = qBound(1, weekday, 7);
        emit currentSetChanged();
    }
}

int BackupSetController::currentScheduleDayOfMonth() const
{
    const BackupSet *set = currentSet();
    return set == nullptr ? 1 : set->schedule.dayOfMonth;
}

void BackupSetController::setCurrentScheduleDayOfMonth(int day)
{
    if (BackupSet *set = currentSet()) {
        set->schedule.dayOfMonth = qBound(1, day, 31);
        emit currentSetChanged();
    }
}

int BackupSetController::currentRetention() const
{
    const BackupSet *set = currentSet();
    return set == nullptr ? 3 : set->retention;
}

void BackupSetController::setCurrentRetention(int retention)
{
    if (BackupSet *set = currentSet()) {
        set->retention = qMax(1, retention);
        emit currentSetChanged();
    }
}

bool BackupSetController::currentOnlyOnAcPower() const
{
    const BackupSet *set = currentSet();
    return set != nullptr && set->onlyOnAcPower;
}

void BackupSetController::setCurrentOnlyOnAcPower(bool enabled)
{
    if (BackupSet *set = currentSet()) {
        set->onlyOnAcPower = enabled;
        emit currentSetChanged();
    }
}

QString BackupSetController::currentNextRun() const
{
    const BackupSet *set = currentSet();
    if (set == nullptr || !set->schedule.enabled()) {
        return QStringLiteral("Not scheduled");
    }

    const QDateTime next = BackupScheduleCalculator::nextRun(set->schedule, QDateTime::currentDateTime());
    return next.isValid() ? next.toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QStringLiteral("Not scheduled");
}

QString BackupSetController::currentRunStatus() const
{
    const BackupSet *set = currentSet();
    const BackupRunRecord *record = set == nullptr ? nullptr : runStore.find(set->id);
    return record == nullptr ? QStringLiteral("idle") : record->status;
}

QString BackupSetController::currentRunError() const
{
    const BackupSet *set = currentSet();
    const BackupRunRecord *record = set == nullptr ? nullptr : runStore.find(set->id);
    return record == nullptr ? QString() : record->lastError;
}

QStringList BackupSetController::runningSetIds() const
{
    return cachedRunningSetIds;
}

QString BackupSetController::currentStagingDirectory() const
{
    const auto *set = currentSet();
    return set ? set->stagingDirectory : QString();
}

qint64 BackupSetController::currentStagingBudget() const
{
    const auto *set = currentSet();
    return set ? set->stagingBudget : 1000 * 1000 * 1000;
}

QStringList BackupSetController::calculateRunningSetIds() const
{
    QStringList ids;
    for (const BackupSet &set : config.sets) {
        const BackupRunRecord *record = runStore.find(set.id);
        if (record != nullptr && isRunActive(record->status)) {
            ids.append(set.id);
        }
    }
    return ids;
}

QVariantMap BackupSetController::remainingTimes() const
{
    return cachedRemainingTimes;
}

QVariantMap BackupSetController::calculateRemainingTimes() const
{
    QVariantMap result;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const BackupSet &set : config.sets) {
        const BackupRunRecord *record = runStore.find(set.id);
        if (record == nullptr || !isRunActive(record->status)) {
            continue;
        }
        QString text;
        if (!record->controlRequest.isEmpty()) {
            text = record->controlRequest == "cancel" ? tr("Cancelling…") : tr("Pausing…");
        } else if (record->unfinished) {
            text = phaseLabel(record->progress.phase) + QStringLiteral("…");
        } else if (record->progress.finalizing || (record->progress.totalFiles > 0
            && record->progress.processedFiles >= record->progress.totalFiles)) {
            text = tr("Finalizing backup…");
        } else {
            const qint64 seconds = record->estimatedRemainingSeconds(now);
            if (seconds < 0) {
                text = tr("Estimating time remaining…");
            } else if (seconds == 0) {
                text = tr("Taking longer than estimated…");
            } else {
                const QString minutesAndSeconds = QStringLiteral("%1:%2")
                    .arg(seconds / 60 % 60, 2, 10, QChar('0'))
                    .arg(seconds % 60, 2, 10, QChar('0'));
                const QString duration = seconds >= 3600
                    ? QStringLiteral("%1:%2").arg(seconds / 3600).arg(minutesAndSeconds)
                    : minutesAndSeconds;
                text = tr("Est. remaining: %1").arg(duration);
            }
        }
        result.insert(set.id, text);
    }
    return result;
}

QStringList BackupSetController::recentBackups() const
{
    return cachedRecentBackups;
}

QVariantMap BackupSetController::transferProgress() const
{
    return cachedTransferProgress;
}

QVariantMap BackupSetController::calculateTransferProgress() const
{
    QVariantMap result;
    for (const BackupSet &set : config.sets) {
        const BackupRunRecord *record = runStore.find(set.id);
        if (record == nullptr || !isRunActive(record->status)) {
            continue;
        }
        const BackupProgress &progress = record->progress;
        QString text = progress.totalFiles > 0
            ? tr("%1 of %2 files processed · %3 verified · %4 items failed")
                .arg(progress.processedFiles).arg(progress.totalFiles).arg(progress.verifiedFiles).arg(progress.failedItems)
            : tr("Preparing backup…");
        if (!progress.currentFile.isEmpty()) {
            text += QStringLiteral("\n") + tr("%1: %2 (%3)").arg(phaseLabel(progress.phase),
                progress.currentFile, QLocale().formattedDataSize(progress.currentFileBytes));
        }
        if (record->unfinished) text += QStringLiteral(" · ") + tr("Verified data: %1").arg(QLocale().formattedDataSize(progress.verifiedBytes));
        result.insert(set.id, QVariantMap {
            {QStringLiteral("text"), text},
            {QStringLiteral("fraction"), progress.totalFiles > 0
                ? qBound(0.0, double(progress.processedFiles) / progress.totalFiles, 1.0) : 0.0},
            {QStringLiteral("indeterminate"), progress.totalFiles <= 0 || progress.finalizing
                || progress.phase == QStringLiteral("staging") || progress.phase == QStringLiteral("uploading-folder")},
        });
    }
    return result;
}

QVariantMap BackupSetController::backupDetails(const QString &setId) const
{
    const BackupRunRecord *record = runStore.find(setId);
    if (record == nullptr) {
        return {};
    }
    QVariantList issues;
    for (const BackupIssue &issue : record->result.issues) {
        issues.append(QVariantMap {{QStringLiteral("path"), issue.path},
            {QStringLiteral("phase"), phaseLabel(issue.phase)}, {QStringLiteral("reason"), issue.reason}});
    }
    // A payload verification alone is insufficient to claim a restorable copy.
    const bool verified = record->result.manifestVerified;
    return {
        {QStringLiteral("status"), statusLabel(record->status)},
        {QStringLiteral("summary"), record->result.reported && verified ? resultSummary(record->result)
            : record->unfinished ? tr("%1 payloads verified · %2 · unfinished copy (manifest pending)")
                .arg(record->progress.verifiedFiles).arg(QLocale().formattedDataSize(record->progress.verifiedBytes)) : QString()},
        {QStringLiteral("error"), record->lastError},
        {QStringLiteral("copyPath"), record->remoteCopyPath},
        {QStringLiteral("currentFile"), isRunActive(record->status) && !record->progress.currentFile.isEmpty()
            ? tr("%1: %2 (%3)").arg(phaseLabel(record->progress.phase), record->progress.currentFile,
                QLocale().formattedDataSize(record->progress.currentFileBytes)) : QString()},
        {QStringLiteral("issues"), issues},
        {QStringLiteral("cancelledCopies"), record->cancelledCopies},
        {QStringLiteral("nextAttempt"), record->nextAttempt.isValid()
            ? record->nextAttempt.toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm:ss")) : QString()},
    };
}

QVariantMap BackupSetController::runDetails() const
{
    QVariantMap details;
    for (const BackupSet &set : config.sets) {
        details.insert(set.id, backupDetails(set.id));
    }
    return details;
}

QStringList BackupSetController::recentBackupSetIds() const
{
    return cachedRecentSetIds;
}

QStringList BackupSetController::recentBackupTimestamps() const
{
    return cachedRecentTimestamps;
}

QStringList BackupSetController::recentBackupCopyPaths() const
{
    QStringList paths;
    for (const QString &setId : cachedRecentSetIds) {
        const BackupRunRecord *record = runStore.find(setId);
        if (record && record->status != QStringLiteral("running")
            && record->status != QStringLiteral("copy_deleted") && !record->remoteCopyPath.isEmpty()) {
            paths.append(record->remoteCopyPath);
        }
    }
    return paths;
}

QString BackupSetController::recentBackupFolderPath(const QString &setId) const
{
    for (const BackupSet &set : config.sets) {
        if (set.id == setId) {
            const BackupRunRecord *record = runStore.find(setId);
            if (record != nullptr && !record->remoteCopyPath.isEmpty()) {
                return QFileInfo(record->remoteCopyPath).path();
            }
            return set.remoteFolder(QSysInfo::machineHostName());
        }
    }
    return {};
}

bool BackupSetController::previewAvailable() const
{
    return hasPreview;
}

QStringList BackupSetController::previewIncluded() const
{
    return previewResult.includedFiles;
}

QStringList BackupSetController::previewExcluded() const
{
    return previewResult.excludedFiles;
}

QStringList BackupSetController::previewSkipped() const
{
    return previewResult.skippedPaths;
}

QStringList BackupSetController::previewMissing() const
{
    return previewResult.missingPaths;
}

QStringList BackupSetController::cleanupTargets() const
{
    const BackupSet *set = currentSet();
    return set == nullptr ? QStringList() : cleanupStore.state(set->id).targets;
}

bool BackupSetController::cleanupConfirmationRequired() const
{
    const BackupSet *set = currentSet();
    if (set == nullptr) {
        return false;
    }
    const CleanupState state = cleanupStore.state(set->id);
    return state.decision != QStringLiteral("confirmed") && !state.targets.isEmpty();
}

void BackupSetController::addSet()
{
    config.sets.append(newSet(config.sets.size() + 1));
    selectedIndex = config.sets.size() - 1;
    unsavedSetIds.insert(currentId());
    emit setsChanged();
    emit currentIndexChanged();
    emit currentSetChanged();
    clearPreview();
}

void BackupSetController::discardUnsavedSet()
{
    if (!unsavedSetIds.remove(currentId())) {
        return;
    }

    config.sets.removeAt(selectedIndex);
    selectedIndex = qMin(selectedIndex, config.sets.size() - 1);
    clearPreview();
    emit setsChanged();
    emit currentIndexChanged();
    emit currentSetChanged();
}

void BackupSetController::removeCurrentSet()
{
    removeSet(selectedIndex);
}

void BackupSetController::removeSet(int index)
{
    if (index < 0 || index >= config.sets.size()) {
        return;
    }

    BackupConfig updated = config;
    updated.sets.removeAt(index);
    QString error;
    if (!store.save(updated, &error)) {
        emit failed(error);
        return;
    }

    config = updated;
    unsavedSetIds.clear();
    if (selectedIndex > index) {
        --selectedIndex;
    } else if (selectedIndex == index) {
        selectedIndex = qMin(selectedIndex, config.sets.size() - 1);
    }
    emit setsChanged();
    emit currentIndexChanged();
    emit currentSetChanged();
    clearPreview();
    emit statusChanged(QStringLiteral("Backup set removed."));
    emit configurationSaved();
}

void BackupSetController::applyCurrentDraft(const QVariantMap &draft)
{
    BackupSet *set = currentSet();
    if (set == nullptr) return;

    BackupSet candidate = *set;
    candidate.name = draft.value(QStringLiteral("name")).toString();
    candidate.remoteRoot = draft.value(QStringLiteral("remoteRoot")).toString();
    candidate.sourceDirectories = draftPaths(draft.value(QStringLiteral("sources")));
    candidate.exclusions = draftPaths(draft.value(QStringLiteral("exclusions")));
    candidate.schedule.frequency = draft.value(QStringLiteral("scheduleFrequency")).toString();
    candidate.schedule.hour = qBound(0, draft.value(QStringLiteral("scheduleHour")).toInt(), 23);
    candidate.schedule.minute = qBound(0, draft.value(QStringLiteral("scheduleMinute")).toInt(), 59);
    candidate.schedule.weekday = qBound(1, draft.value(QStringLiteral("scheduleWeekday")).toInt(), 7);
    candidate.schedule.dayOfMonth = qBound(1, draft.value(QStringLiteral("scheduleDayOfMonth")).toInt(), 31);
    candidate.retention = qMax(1, draft.value(QStringLiteral("retention")).toInt());
    candidate.onlyOnAcPower = draft.value(QStringLiteral("onlyOnAcPower")).toBool();
    if (draft.contains("stagingDirectory")) candidate.stagingDirectory = draft.value("stagingDirectory").toString().trimmed();
    if (draft.contains("stagingBudget")) candidate.stagingBudget = draft.value("stagingBudget").toLongLong();

    const bool nameChanged = candidate.name != set->name;
    const bool previewInputsChanged = candidate.sourceDirectories != set->sourceDirectories
        || candidate.exclusions != set->exclusions;
    if (!nameChanged && !previewInputsChanged && candidate.remoteRoot == set->remoteRoot
        && candidate.schedule.frequency == set->schedule.frequency
        && candidate.schedule.hour == set->schedule.hour
        && candidate.schedule.minute == set->schedule.minute
        && candidate.schedule.weekday == set->schedule.weekday
        && candidate.schedule.dayOfMonth == set->schedule.dayOfMonth
        && candidate.retention == set->retention && candidate.onlyOnAcPower == set->onlyOnAcPower
        && candidate.stagingDirectory == set->stagingDirectory && candidate.stagingBudget == set->stagingBudget) {
        return;
    }

    // Publish the finished set before notifying any observer, including preview observers.
    *set = candidate;
    if (previewInputsChanged) clearPreview();
    if (nameChanged) emit setsChanged();
    emit currentSetChanged();
}

void BackupSetController::preview()
{
    const BackupSet *set = currentSet();
    if (set == nullptr) {
        clearPreview();
        emit failed(QStringLiteral("A backup must be selected."));

        return;
    }

    pendingSources = set->sourceDirectories;
    pendingExclusions = set->exclusions;
    if (previewCancelled) previewCancelled->store(true);
    ++previewGeneration;
    if (!previewWorking) {
        previewWorking = true;
        emit previewBusyChanged();
    }
    if (!scanInFlight) startPreview();
}

void BackupSetController::startPreview()
{
    activePreviewGeneration = previewGeneration;
    scanInFlight = true;
    const auto sources = pendingSources;
    const auto exclusions = pendingExclusions;
    BackupEngine *worker = &engine;
    previewCancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = previewCancelled;
    previewWatcher.setFuture(QtConcurrent::run([worker, sources, exclusions, cancelled] {
        return worker->preview(sources, exclusions, [cancelled] { return cancelled->load(); });
    }));
}

bool BackupSetController::save()
{
    const BackupSet *set = currentSet();
    if (set == nullptr || set->name.trimmed().isEmpty() || set->remoteRoot.trimmed().isEmpty()
        || set->sourceDirectories.isEmpty()) {
        emit failed(QStringLiteral("Enter a backup name, choose at least one source, and check the destination in Advanced settings."));

        return false;
    }

    QString error;
    if (set->stagingBudget <= 0 || (!set->stagingDirectory.isEmpty()
        && !BackupStaging::validate(set->stagingDirectory, set->sourceDirectories, &error))) {
        emit failed(error.isEmpty() ? tr("Enter a positive staging budget.") : error);
        return false;
    }
    if (!store.save(config, &error)) {
        emit failed(error);

        return false;
    }

    unsavedSetIds.clear();
    emit statusChanged(QStringLiteral("Backup saved."));
    emit configurationSaved();
    return true;
}

bool BackupSetController::isLocalStatePath(const QString &filePath) const
{
    const QFileInfo destination(filePath);
    for (const QString &statePath : {store.filePath(), runStore.filePath(), cleanupStore.filePath()}) {
        const QFileInfo state(statePath);
        if (QDir::cleanPath(destination.absoluteFilePath()) == QDir::cleanPath(state.absoluteFilePath())
            || (!destination.canonicalFilePath().isEmpty() && destination.canonicalFilePath() == state.canonicalFilePath())) {
            return true;
        }
    }
    return false;
}

bool BackupSetController::exportSets(const QString &filePath)
{
    if (isLocalStatePath(filePath)) {
        emit failed(QStringLiteral("Choose a separate file for exporting your backup sets."));
        return false;
    }
    BackupConfig saved;
    QString error;
    if (!store.load(&saved, &error) || !BackupConfigStore(filePath).exportSets(saved, &error)) {
        emit failed(error);
        return false;
    }
    emit statusChanged(QStringLiteral("Backup sets exported."));
    return true;
}

bool BackupSetController::saveTemplate(const QString &filePath)
{
    if (isLocalStatePath(filePath)) {
        emit failed(QStringLiteral("Choose a separate file for saving the backup-set template."));
        return false;
    }
    Q_INIT_RESOURCE(backup_set_template);
    QFile source(QStringLiteral(":/omacustos/backup-sets.template.json"));
    if (!source.open(QIODevice::ReadOnly)) {
        emit failed(QStringLiteral("The bundled backup-set template could not be opened."));
        return false;
    }
    const QByteArray contents = source.readAll();
    QSaveFile destination(filePath);
    if (!destination.open(QIODevice::WriteOnly) || destination.write(contents) != contents.size() || !destination.commit()) {
        emit failed(QStringLiteral("Unable to save the backup-set template: %1").arg(destination.errorString()));
        return false;
    }
    emit statusChanged(QStringLiteral("Backup-set template saved."));
    return true;
}

bool BackupSetController::importSets(const QString &filePath, bool merge)
{
    BackupConfig imported;
    QString error;
    if (!BackupConfigStore(filePath).importSets(&imported, &error)) {
        emit failed(error);
        return false;
    }
    QLockFile managementGate(runStore.filePath() + QStringLiteral(".management.lock"));
    QLockFile workerLock(store.filePath() + QStringLiteral(".worker.lock"));
    QLockFile queueWorkerLock(runStore.filePath() + QStringLiteral(".worker.lock"));
    QLockFile runLock(runStore.filePath() + QStringLiteral(".lock"));
    workerLock.setStaleLockTime(0);
    queueWorkerLock.setStaleLockTime(0);
    runLock.setStaleLockTime(0);
    managementGate.setStaleLockTime(0);
    if (!managementGate.tryLock(0) || !workerLock.tryLock(0) || !queueWorkerLock.tryLock(0) || !runLock.tryLock(0)) {
        emit failed(QStringLiteral("Wait for the current backup or queue update to finish before importing sets."));
        return false;
    }
    BackupConfig updated = config;
    if (merge) {
        updated.sets.erase(std::remove_if(updated.sets.begin(), updated.sets.end(), [this](const BackupSet &set) {
            return unsavedSetIds.contains(set.id);
        }), updated.sets.end());
        for (const BackupSet &set : imported.sets) {
            const auto existing = std::find_if(updated.sets.begin(), updated.sets.end(), [&set](const BackupSet &candidate) {
                return candidate.id == set.id;
            });
            if (existing == updated.sets.end()) {
                updated.sets.append(set);
            } else {
                *existing = set;
            }
        }
    } else {
        updated.sets = imported.sets;
    }
    if (!store.save(updated, &error)) {
        emit failed(error);
        return false;
    }
    config = updated;
    unsavedSetIds.clear();
    selectedIndex = config.sets.isEmpty() ? -1 : 0;
    clearPreview();
    emit setsChanged();
    emit currentIndexChanged();
    emit currentSetChanged();
    emit statusChanged(QStringLiteral("Backup sets imported."));
    emit configurationSaved();
    return true;
}

bool BackupSetController::confirmCleanup()
{
    const BackupSet *set = currentSet();
    if (set == nullptr || cleanupStore.state(set->id).targets.isEmpty()) {
        return false;
    }
    const CleanupState presented = cleanupStore.state(set->id);
    QString error;
    if (!cleanupStore.confirm(set->id, presented, &error)) {
        emit cleanupChanged();
        emit failed(error);
        return false;
    }
    emit cleanupChanged();
    emit statusChanged(QStringLiteral("Retention cleanup confirmed; it will run after the next verified backup."));
    return true;
}

BackupSet *BackupSetController::currentSet()
{
    return selectedIndex >= 0 && selectedIndex < config.sets.size()
        ? &config.sets[selectedIndex]
        : nullptr;
}

const BackupSet *BackupSetController::currentSet() const
{
    return selectedIndex >= 0 && selectedIndex < config.sets.size()
        ? &config.sets.at(selectedIndex)
        : nullptr;
}

QVector<int> BackupSetController::recentBackupIndexes() const
{
    QVector<int> indexes;
    indexes.reserve(config.sets.size());
    for (int index = 0; index < config.sets.size(); ++index) {
        const BackupRunRecord *record = runStore.find(config.sets.at(index).id);
        if (record == nullptr || record->status != QStringLiteral("copy_deleted")) {
            indexes.append(index);
        }
    }

    const auto latestActivity = [this](int index) {
        const BackupRunRecord *record = runStore.find(config.sets.at(index).id);
        if (record == nullptr) {
            return QDateTime();
        }

        QDateTime latest = record->lastSuccess;
        if (record->lastFailure > latest) {
            latest = record->lastFailure;
        }
        if (record->lastScheduled > latest) {
            latest = record->lastScheduled;
        }
        return latest;
    };

    std::stable_sort(indexes.begin(), indexes.end(), [&latestActivity](int left, int right) {
        return latestActivity(left) > latestActivity(right);
    });
    return indexes;
}

void BackupSetController::clearPreview()
{
    if (previewCancelled) previewCancelled->store(true);
    ++previewGeneration;
    if (previewWorking) {
        previewWorking = false;
        emit previewBusyChanged();
    }
    previewResult = {};
    hasPreview = false;
    emit previewChanged();
}

void BackupSetController::refreshRunState()
{
    QString runError, cleanupError;
    QByteArray nextRunContents, nextCleanupContents;
    if (readState(runStore.filePath(), &nextRunContents, &runError)
        && nextRunContents != runContents) {
        bool loaded = true;
        if (nextRunContents == QByteArray(1, '\0')) {
            runStore.records().clear();
        } else {
            loaded = runStore.loadFromBytes(nextRunContents.mid(1), &runError);
        }
        if (loaded) {
            runContents = nextRunContents;
            emit runStateChanged();
            emit runDetailsChanged();
        }
    }
    if (readState(cleanupStore.filePath(), &nextCleanupContents, &cleanupError)
        && nextCleanupContents != cleanupContents) {
        bool loaded = true;
        if (nextCleanupContents == QByteArray(1, '\0')) {
            cleanupStore.loadFromBytes("{}");
        } else {
            loaded = cleanupStore.loadFromBytes(nextCleanupContents.mid(1), &cleanupError);
        }
        if (loaded) {
            cleanupContents = nextCleanupContents;
            emit cleanupChanged();
        }
    }
    updateDashboard();
    stateTimer.setInterval(cachedRunningSetIds.isEmpty() ? 5000 : 1000);
    const QString nextError = (runError + QStringLiteral(" ") + cleanupError).trimmed();
    if (refreshError != nextError) {
        refreshError = nextError;
        emit dashboardRefreshChanged();
    }
}

void BackupSetController::updateDashboard()
{
    const auto running = calculateRunningSetIds();
    const auto remaining = calculateRemainingTimes();
    const auto transfer = calculateTransferProgress();
    QStringList summaries, ids, timestamps;
    QVariantMap runSummaries, setSummaries;
    const QDateTime now = QDateTime::currentDateTime();
    const auto timestamp = [](const QDateTime &date) {
        return date.isValid() ? date.toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm:ss")) : QString();
    };
    for (const BackupSet &set : config.sets) {
        const QString time = QStringLiteral("%1:%2").arg(set.schedule.hour, 2, 10, QChar('0'))
            .arg(set.schedule.minute, 2, 10, QChar('0'));
        QString schedule = tr("Manual backups");
        if (set.schedule.frequency == QStringLiteral("daily")) schedule = tr("Daily at %1").arg(time);
        if (set.schedule.frequency == QStringLiteral("weekly")) schedule = tr("Every %1 at %2")
            .arg(QLocale().standaloneDayName(set.schedule.weekday), time);
        if (set.schedule.frequency == QStringLiteral("monthly")) schedule = tr("Monthly on day %1 at %2")
            .arg(set.schedule.dayOfMonth).arg(time);
        const QDateTime next = set.schedule.enabled() ? BackupScheduleCalculator::nextRun(set.schedule, now) : QDateTime();
        setSummaries.insert(set.id, QVariantMap {
            {QStringLiteral("sourceCount"), set.sourceDirectories.size()},
            {QStringLiteral("schedule"), schedule},
            {QStringLiteral("nextRun"), next.isValid() ? next.toString(QStringLiteral("dd/MM/yyyy HH:mm")) : QString()},
            {QStringLiteral("retention"), set.retention},
            {QStringLiteral("onlyOnAcPower"), set.onlyOnAcPower},
        });
        const BackupRunRecord *record = runStore.find(set.id);
        if (record == nullptr) continue;
        const bool hasActivity = record->lastSuccess.isValid() || record->lastFailure.isValid()
            || !record->remoteCopyPath.isEmpty() || record->status == QStringLiteral("copy_deleted");
        const bool latestCopy = record->status != QStringLiteral("copy_deleted")
            && (!record->remoteCopyPath.isEmpty() || record->lastSuccess.isValid());
        runSummaries.insert(set.id, QVariantMap {
            {QStringLiteral("status"), statusLabel(record->status)},
            {QStringLiteral("statusCode"), record->status},
            {QStringLiteral("summary"), record->result.reported && record->result.manifestVerified
                && record->status != QStringLiteral("copy_deleted") ? resultSummary(record->result)
                : record->unfinished ? tr("%1 payloads verified · %2 · unfinished copy (manifest pending)")
                    .arg(record->progress.verifiedFiles).arg(QLocale().formattedDataSize(record->progress.verifiedBytes)) : QString()},
            {QStringLiteral("lastAttempt"), timestamp(qMax(record->lastSuccess, record->lastFailure))},
            {QStringLiteral("lastSuccess"), timestamp(record->lastSuccess)},
            {QStringLiteral("error"), record->lastError},
            {QStringLiteral("hasActivity"), hasActivity},
            {QStringLiteral("hasLatestCopy"), latestCopy},
            {QStringLiteral("unfinished"), record->unfinished},
            {QStringLiteral("controlRequest"), record->controlRequest},
        });
    }
    // Sort once per refresh, and keep expensive issue conversion out of row bindings.
    for (const int index : recentBackupIndexes()) {
        const BackupSet &set = config.sets.at(index);
        ids.append(set.id);
        const BackupRunRecord *record = runStore.find(set.id);
        if (record == nullptr) {
            summaries.append(QStringLiteral("%1\nNo backup run yet").arg(set.name));
            timestamps.append(QString());
            continue;
        }
        QString detail = statusLabel(record->status);
        if (!record->result.manifestVerified && !record->lastError.isEmpty()) {
            detail += QStringLiteral(" | %1").arg(record->lastError);
        }
        summaries.append(QStringLiteral("%1\n%2").arg(set.name, detail));
        const QDateTime latest = qMax(record->lastSuccess, qMax(record->lastFailure, record->lastScheduled));
        timestamps.append(latest.isValid()
            ? latest.toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm:ss")) : QString());
    }
    const bool runningChanged = running != cachedRunningSetIds;
    const bool remainingChanged = remaining != cachedRemainingTimes;
    const bool transferChanged = transfer != cachedTransferProgress;
    const bool recentChanged = summaries != cachedRecentBackups || ids != cachedRecentSetIds
        || timestamps != cachedRecentTimestamps || runSummaries != cachedRunSummaries
        || setSummaries != cachedSetSummaries;
    cachedRunningSetIds = running;
    cachedRemainingTimes = remaining;
    cachedTransferProgress = transfer;
    cachedRecentBackups = summaries;
    cachedRecentSetIds = ids;
    cachedRecentTimestamps = timestamps;
    cachedRunSummaries = runSummaries;
    cachedSetSummaries = setSummaries;
    if (runningChanged) emit runningSetIdsChanged();
    if (remainingChanged) emit remainingTimesChanged();
    if (transferChanged) emit transferProgressChanged();
    if (recentChanged) emit dashboardChanged();
}

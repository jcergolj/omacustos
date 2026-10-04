#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QSemaphore>
#include <QScopeGuard>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrentRun>

#include "../src/backupsetcontroller.h"

namespace {

QVariantMap currentDraft(const BackupSetController &controller)
{
    return {
        {"name", controller.currentName()}, {"remoteRoot", controller.currentRemoteRoot()},
        {"sources", controller.currentSources()}, {"exclusions", controller.currentExclusions()},
        {"scheduleFrequency", controller.currentScheduleFrequency()},
        {"scheduleHour", controller.currentScheduleHour()}, {"scheduleMinute", controller.currentScheduleMinute()},
        {"scheduleWeekday", controller.currentScheduleWeekday()},
        {"scheduleDayOfMonth", controller.currentScheduleDayOfMonth()},
        {"retention", controller.currentRetention()}, {"onlyOnAcPower", controller.currentOnlyOnAcPower()},
    };
}

}

class BackupSetControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void completeDraftPublishesCoherentStateAndSkipsUnchangedUpdates();
    void draftPreservesOrInvalidatesInFlightPreview_data();
    void draftPreservesOrInvalidatesInFlightPreview();
    void previewUpdatesFilesWithoutCountMessage_data();
    void previewUpdatesFilesWithoutCountMessage();
    void previewGroupsAreReadOnlyAndResetWithSelection_data();
    void previewGroupsAreReadOnlyAndResetWithSelection();
    void previewKeepsInputAvailableAndDiscardsSupersededSelection();
    void failedDashboardRefreshRetainsLastSuccessfulData_data();
    void failedDashboardRefreshRetainsLastSuccessfulData();
    void missingStateFilesClearDashboardSnapshots();
    void unchangedPollingDoesNotResetDashboardAndProgressUpdatesStaySeparate();
    void recentBackupTimestampIncludesLocalDateAndSeconds();
    void folderPathUsesBackupIdentityAndWorkerNaming();
    void deletedCopyDisappearsFromRecentBackupsButKeepsItsSet();
    void exportsSavedSetsAndImportsTheirSettings();
    void importsAnnotatedTemplate();
    void savesBundledTemplateWithoutChangingConfiguration();
    void templateCannotOverwriteLocalStateOrReportFailedWritesAsSuccess();
    void mergesSetsByIdAndPreservesOtherSets();
    void emptyMergeKeepsExistingSets();
    void summarizesIndependentCardSettingsWithoutChangingSelection();
    void retainsCopyNavigationAfterFailureAndLatestCopyDeletion();
    void invalidImportLeavesExistingSetsUntouched_data();
    void invalidImportLeavesExistingSetsUntouched();
    void emptyImportDoesNotRestoreLegacySources();
    void removingFinalSetPersistsEmptyConfiguration_data();
    void removingFinalSetPersistsEmptyConfiguration();
    void exportCannotOverwriteLocalState();
    void importIsBlockedWhileWorkerRuns();
    void successfulSaveNotifiesSchedulingButPreviewAndFailedSaveDoNot();
    void discardingUnsavedSetDoesNotWriteConfiguration_data();
    void discardingUnsavedSetDoesNotWriteConfiguration();
    void savedSetIsKeptWhenEditorIsCancelled();
    void mergingImportDoesNotSaveAnUnfinishedNewSet();
    void remainingTimeIsReportedForTheRunningSetOnly();
    void reportsVerifiedCountsAndFailureDetailsWithoutInventingLegacyCounts();
};

void BackupSetControllerTest::completeDraftPublishesCoherentStateAndSkipsUnchangedUpdates()
{
    QTemporaryDir home;
    const QString settings = home.filePath("settings.json");
    BackupConfig config;
    config.protonBinary = "/custom/proton-drive";
    config.sets = {{"documents", "Documents", "/backups", {"/safe/documents"}, {}},
                   {"photos", "Photos", "/photos", {"/safe/photos"}, {}}};
    QVERIFY(BackupConfigStore(settings).save(config));
    BackupEngine engine;
    BackupSetController controller(engine, settings);
    controller.preview();
    QTRY_VERIFY(controller.previewAvailable());

    const QVariantMap draft {
        {"name", " Updated documents "}, {"remoteRoot", " /new-backups "},
        {"sources", QStringList {" /safe/new ", " ", "/safe/notes.txt"}},
        {"exclusions", QStringList {" cache ", "", " /safe/new/excluded "}},
        {"scheduleFrequency", "monthly"}, {"scheduleHour", 99}, {"scheduleMinute", -1},
        {"scheduleWeekday", 99}, {"scheduleDayOfMonth", 99},
        {"retention", 0}, {"onlyOnAcPower", true},
    };
    QVariantMap expected = draft;
    expected["sources"] = QStringList {"/safe/new", "/safe/notes.txt"};
    expected["exclusions"] = QStringList {"cache", "/safe/new/excluded"};
    expected["scheduleHour"] = 23;
    expected["scheduleMinute"] = 0;
    expected["scheduleWeekday"] = 7;
    expected["scheduleDayOfMonth"] = 31;
    expected["retention"] = 1;
    QSignalSpy current(&controller, &BackupSetController::currentSetChanged);
    QSignalSpy sets(&controller, &BackupSetController::setsChanged);
    QSignalSpy dashboard(&controller, &BackupSetController::dashboardChanged);
    QSignalSpy preview(&controller, &BackupSetController::previewChanged);
    QSignalSpy saved(&controller, &BackupSetController::configurationSaved);
    const auto checkCoherentState = [&] {
        QCOMPARE(currentDraft(controller), expected);
        QCOMPARE(controller.currentId(), QString("documents"));
        QCOMPARE(controller.currentIndex(), 0);
        QVERIFY(!controller.previewAvailable());
    };
    connect(&controller, &BackupSetController::setsChanged, this, checkCoherentState);
    connect(&controller, &BackupSetController::currentSetChanged, this, checkCoherentState);
    connect(&controller, &BackupSetController::dashboardChanged, this, checkCoherentState);
    connect(&controller, &BackupSetController::previewChanged, this, checkCoherentState);
    // QML arrays arrive as QVariantLists rather than QStringLists.
    controller.applyCurrentDraft(QJsonObject::fromVariantMap(draft).toVariantMap());
    QCOMPARE(currentDraft(controller), expected);
    QCOMPARE(current.count(), 1);
    QCOMPARE(sets.count(), 1);
    QCOMPARE(dashboard.count(), 1);
    QCOMPARE(preview.count(), 1);
    QVERIFY(saved.isEmpty());
    BackupConfig persisted;
    QVERIFY(BackupConfigStore(settings).load(&persisted));
    QCOMPARE(persisted.sets.first().name, QString("Documents"));

    controller.applyCurrentDraft(draft);
    controller.applyCurrentDraft(expected);
    QCOMPARE(current.count(), 1);
    QCOMPARE(sets.count(), 1);
    QCOMPARE(dashboard.count(), 1);
    QCOMPARE(preview.count(), 1);
    QVERIFY(saved.isEmpty());
    QVERIFY(controller.save());
    QCOMPARE(saved.count(), 1);
    BackupSetController reopened(engine, settings);
    QCOMPARE(currentDraft(reopened), expected);
    QVERIFY(BackupConfigStore(settings).load(&persisted));
    QCOMPARE(persisted.protonBinary, config.protonBinary);
    QCOMPARE(persisted.sets.at(1).name, config.sets.at(1).name);
    QCOMPARE(persisted.sets.at(1).sourceDirectories, config.sets.at(1).sourceDirectories);
}

void BackupSetControllerTest::draftPreservesOrInvalidatesInFlightPreview_data()
{
    QTest::addColumn<QString>("change");
    QTest::newRow("unrelated settings") << QString("settings");
    QTest::newRow("sources") << QString("sources");
    QTest::newRow("exclusions") << QString("exclusions");
    QTest::newRow("sources and exclusions") << QString("both");
}

void BackupSetControllerTest::draftPreservesOrInvalidatesInFlightPreview()
{
    QFETCH(QString, change);
    QTemporaryDir home;
    QFile source(home.filePath("notes.txt"));
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("notes");
    source.close();
    BackupEngine engine;
    BackupSetController controller(engine, home.filePath("settings.json"));
    controller.addSet();
    controller.setCurrentSources({source.fileName()});
    controller.preview();
    QTRY_VERIFY(controller.previewAvailable());
    QCOMPARE(controller.previewIncluded(), QStringList {source.fileName()});

    QThreadPool *pool = QThreadPool::globalInstance();
    const int previousLimit = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore entered, release;
    auto blocked = QtConcurrent::run([&] { entered.release(); release.acquire(); });
    entered.acquire();
    const auto unblock = qScopeGuard([&] {
        release.release();
        blocked.waitForFinished();
        pool->setMaxThreadCount(previousLimit);
    });
    controller.preview();
    QVERIFY(controller.previewBusy());
    QSignalSpy preview(&controller, &BackupSetController::previewChanged);
    QSignalSpy busy(&controller, &BackupSetController::previewBusyChanged);
    QSignalSpy sets(&controller, &BackupSetController::setsChanged);
    QSignalSpy current(&controller, &BackupSetController::currentSetChanged);
    QVariantMap draft = currentDraft(controller);
    draft["remoteRoot"] = "/new-backups";
    draft["scheduleFrequency"] = "daily";
    draft["retention"] = 7;
    draft["onlyOnAcPower"] = true;
    const bool sourcesChanged = change == "sources" || change == "both";
    const bool exclusionsChanged = change == "exclusions" || change == "both";
    const bool inputsChanged = sourcesChanged || exclusionsChanged;
    if (sourcesChanged) draft["sources"] = QStringList {home.filePath("missing.txt")};
    if (exclusionsChanged) draft["exclusions"] = QStringList {source.fileName()};
    controller.applyCurrentDraft(draft);
    QCOMPARE(current.count(), 1);
    QVERIFY(sets.isEmpty());
    QCOMPARE(preview.count(), inputsChanged ? 1 : 0);
    QCOMPARE(busy.count(), inputsChanged ? 1 : 0);
    QCOMPARE(controller.previewAvailable(), !inputsChanged);
    QCOMPARE(controller.previewBusy(), !inputsChanged);
    controller.applyCurrentDraft(draft);
    QCOMPARE(current.count(), 1);
    QCOMPARE(preview.count(), inputsChanged ? 1 : 0);
    QCOMPARE(busy.count(), inputsChanged ? 1 : 0);

    // A superseded scan must never publish its old selection when it finishes.
    connect(&controller, &BackupSetController::previewChanged, this, [&] {
        if (inputsChanged && controller.previewAvailable()) QVERIFY(controller.previewIncluded().isEmpty());
    });
    if (inputsChanged) controller.preview();
    release.release();
    QTRY_VERIFY(!controller.previewBusy());
    QVERIFY(controller.previewAvailable());
    QCOMPARE(preview.count(), inputsChanged ? 2 : 1);
    QCOMPARE(controller.previewIncluded(), inputsChanged ? QStringList {} : QStringList {source.fileName()});
    QCOMPARE(controller.previewMissing(), sourcesChanged ? QStringList {home.filePath("missing.txt")} : QStringList {});
    QCOMPARE(controller.previewExcluded(), !sourcesChanged && exclusionsChanged ? QStringList {source.fileName()} : QStringList {});
}

void BackupSetControllerTest::unchangedPollingDoesNotResetDashboardAndProgressUpdatesStaySeparate()
{
    QTemporaryDir home;
    BackupConfig config;
    config.sets = {{"documents", "Documents", "/backups", {"/safe/documents"}, {}}};
    const QString settings = home.filePath("settings.json");
    QVERIFY(BackupConfigStore(settings).save(config));
    BackupRunStore runs(home.filePath("omacustos-backup-runs.json"));
    runs.ensureSet("documents");
    auto &record = *runs.find("documents");
    runs.markRunning(record);
    record.progress = {100, 10, 10, 1, false};
    record.progressElapsedMs = 1000;
    record.progressUpdatedAt = QDateTime::currentDateTimeUtc();
    QVERIFY(runs.save());
    BackupEngine engine;
    BackupSetController controller(engine, settings);
    QSignalSpy dashboard(&controller, &BackupSetController::dashboardChanged);
    QSignalSpy state(&controller, &BackupSetController::runStateChanged);
    QSignalSpy transfer(&controller, &BackupSetController::transferProgressChanged);
    QSignalSpy cleanup(&controller, &BackupSetController::cleanupChanged);
    QSignalSpy details(&controller, &BackupSetController::runDetailsChanged);
    QSignalSpy remaining(&controller, &BackupSetController::remainingTimesChanged);
    controller.refreshRunState();
    controller.refreshRunState();
    QVERIFY(dashboard.isEmpty() && state.isEmpty() && transfer.isEmpty() && cleanup.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!remaining.isEmpty(), 2500);
    QVERIFY(dashboard.isEmpty() && state.isEmpty() && transfer.isEmpty() && cleanup.isEmpty() && details.isEmpty());
    record.progress.processedFiles = 2;
    record.progress.processedBytes = 20;
    QVERIFY(runs.save());
    controller.refreshRunState();
    QCOMPARE(transfer.count(), 1);
    QCOMPARE(state.count(), 1);
    QVERIFY(dashboard.isEmpty());
    QCOMPARE(controller.transferProgress().value("documents").toMap().value("fraction").toDouble(), 0.2);
    runs.markSuccess(record, QDateTime::currentDateTimeUtc());
    QVERIFY(runs.save());
    controller.refreshRunState();
    QCOMPARE(dashboard.count(), 1);
    QVERIFY(controller.runningSetIds().isEmpty());
    QCOMPARE(controller.recentBackups().first(), QString("Documents\nSuccessful"));
    // An empty file differs from an absent file, and failures retain the snapshot.
    QFile file(runs.filePath());
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.close();
    controller.refreshRunState();
    QVERIFY(!controller.dashboardRefreshError().isEmpty());
    QCOMPARE(dashboard.count(), 1);
    QVERIFY(runs.save());
    controller.refreshRunState();
    QVERIFY(controller.dashboardRefreshError().isEmpty());
    QCOMPARE(dashboard.count(), 1);
}

void BackupSetControllerTest::failedDashboardRefreshRetainsLastSuccessfulData_data()
{
    QTest::addColumn<bool>("corruptCleanup");
    QTest::addColumn<bool>("unreadable");
    for (bool cleanup : {false, true}) {
        const QString prefix = cleanup ? "cleanup: " : "runs: ";
        QTest::newRow(qPrintable(prefix + "invalid record after valid record")) << cleanup << false;
        QTest::newRow(qPrintable(prefix + "unreadable file")) << cleanup << true;
    }
}

void BackupSetControllerTest::failedDashboardRefreshRetainsLastSuccessfulData()
{
    QFETCH(bool, corruptCleanup);
    QFETCH(bool, unreadable);
    QTemporaryDir home;
    BackupConfig config;
    config.sets = {{"documents", "Documents", "/backups", {"/safe/documents"}, {}}};
    const QString settings = home.filePath("settings.json");
    QVERIFY(BackupConfigStore(settings).save(config));
    BackupRunStore runs(home.filePath("omacustos-backup-runs.json"));
    runs.ensureSet("documents");
    runs.markSuccess(*runs.find("documents"), QDateTime::currentDateTimeUtc());
    QVERIFY(runs.save());
    CleanupStore cleanup(home.filePath("omacustos-backup-cleanup.json"));
    cleanup.setPending("documents", {"/backups/old"});
    QVERIFY(cleanup.save());
    BackupEngine engine;
    BackupSetController controller(engine, settings);
    const auto timestamps = controller.recentBackupTimestamps();
    const auto summaries = controller.runSummaries();
    const auto targets = controller.cleanupTargets();
    QVERIFY(!targets.isEmpty());
    QSignalSpy dashboard(&controller, &BackupSetController::dashboardChanged);
    QSignalSpy stateChanged(&controller, &BackupSetController::runStateChanged);
    QSignalSpy cleanupChanged(&controller, &BackupSetController::cleanupChanged);
    const QString path = corruptCleanup ? cleanup.filePath() : runs.filePath();
    if (unreadable) {
        QVERIFY(QFile::remove(path));
        QVERIFY(QDir().mkdir(path));
    } else {
        QFile state(path);
        QVERIFY(state.open(QIODevice::WriteOnly | QIODevice::Truncate));
        state.write(corruptCleanup
            ? R"({"sets":{"a-valid":{},"z-invalid":false}})"
            : R"({"runs":[{"set_id":"new"},{}]})");
    }
    controller.refreshRunState();
    QCOMPARE(controller.recentBackupTimestamps(), timestamps);
    QCOMPARE(controller.runSummaries(), summaries);
    QCOMPARE(controller.cleanupTargets(), targets);
    QVERIFY(controller.cleanupConfirmationRequired());
    QVERIFY(!controller.dashboardRefreshError().isEmpty());
    const QString error = controller.dashboardRefreshError();
    controller.refreshRunState();
    QCOMPARE(controller.dashboardRefreshError(), error);
    QVERIFY(dashboard.isEmpty() && stateChanged.isEmpty() && cleanupChanged.isEmpty());
    if (unreadable) QVERIFY(QDir().rmdir(path));
    QVERIFY(runs.save());
    QVERIFY(cleanup.save());
    controller.refreshRunState();
    QVERIFY(controller.dashboardRefreshError().isEmpty());
    QCOMPARE(controller.recentBackupTimestamps(), timestamps);
    QCOMPARE(controller.cleanupTargets(), targets);
    QVERIFY(dashboard.isEmpty() && stateChanged.isEmpty() && cleanupChanged.isEmpty());
}

void BackupSetControllerTest::missingStateFilesClearDashboardSnapshots()
{
    QTemporaryDir home;
    BackupConfig config;
    config.sets = {{"documents", "Documents", "/backups", {"/safe/documents"}, {}}};
    const QString settings = home.filePath("settings.json");
    QVERIFY(BackupConfigStore(settings).save(config));
    BackupRunStore runs(home.filePath("omacustos-backup-runs.json"));
    runs.ensureSet("documents");
    runs.markSuccess(*runs.find("documents"), QDateTime::currentDateTimeUtc());
    QVERIFY(runs.save());
    CleanupStore cleanup(home.filePath("omacustos-backup-cleanup.json"));
    cleanup.setPending("documents", {"/backups/old"});
    QVERIFY(cleanup.save());
    BackupEngine engine;
    BackupSetController controller(engine, settings);
    QVERIFY(!controller.runSummaries().isEmpty());
    QVERIFY(!controller.cleanupTargets().isEmpty());
    QSignalSpy stateChanged(&controller, &BackupSetController::runStateChanged);
    QSignalSpy cleanupChanged(&controller, &BackupSetController::cleanupChanged);
    QVERIFY(QFile::remove(runs.filePath()));
    QVERIFY(QFile::remove(cleanup.filePath()));
    controller.refreshRunState();
    QVERIFY(controller.runSummaries().isEmpty());
    QCOMPARE(controller.recentBackups(), QStringList {"Documents\nNo backup run yet"});
    QCOMPARE(controller.recentBackupTimestamps(), QStringList {QString()});
    QVERIFY(controller.cleanupTargets().isEmpty());
    QVERIFY(controller.dashboardRefreshError().isEmpty());
    QCOMPARE(stateChanged.count(), 1);
    QCOMPARE(cleanupChanged.count(), 1);
    controller.refreshRunState();
    QCOMPARE(stateChanged.count(), 1);
    QCOMPARE(cleanupChanged.count(), 1);
    QFile empty(cleanup.filePath());
    QVERIFY(empty.open(QIODevice::WriteOnly));
    empty.close();
    controller.refreshRunState();
    QVERIFY(!controller.dashboardRefreshError().isEmpty());
    QCOMPARE(cleanupChanged.count(), 1);
    QVERIFY(QFile::remove(cleanup.filePath()));
    controller.refreshRunState();
    QVERIFY(controller.dashboardRefreshError().isEmpty());
}

void BackupSetControllerTest::previewKeepsInputAvailableAndDiscardsSupersededSelection()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    QFile source(home.filePath("notes.txt"));
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("notes");
    source.close();
    BackupEngine engine;
    // Hold the executor so the scan is deterministically pending while the UI
    // changes selection. Assert public results and event-loop availability.
    QThreadPool *pool = QThreadPool::globalInstance();
    const int previousLimit = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore entered, release;
    auto blocked = QtConcurrent::run([&] { entered.release(); release.acquire(); });
    entered.acquire();
    BackupSetController controller(engine, home.filePath("settings.json"));
    const auto unblock = qScopeGuard([&] {
        release.release();
        blocked.waitForFinished();
        pool->setMaxThreadCount(previousLimit);
    });
    controller.addSet();
    controller.setCurrentSources({source.fileName()});
    controller.preview();
    QVERIFY(!controller.previewAvailable());
    QVERIFY(controller.property("previewBusy").toBool());
    bool heartbeat = false;
    QTimer::singleShot(0, &controller, [&] { heartbeat = true; });
    QTRY_VERIFY(heartbeat);
    controller.addSet();
    controller.setCurrentSources({home.filePath("missing.txt")});
    controller.preview();
    QVERIFY(!controller.previewAvailable());
    release.release();
    QTRY_VERIFY(controller.previewAvailable());
    QVERIFY(!controller.property("previewBusy").toBool());
    QVERIFY(controller.previewIncluded().isEmpty());
    QCOMPARE(controller.previewMissing(), QStringList {home.filePath("missing.txt")});
    controller.setCurrentSources({source.fileName()});
    QVERIFY(!controller.previewAvailable());
}

void BackupSetControllerTest::remainingTimeIsReportedForTheRunningSetOnly()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString configPath = home.filePath("settings.json");
    BackupConfig config;
    config.sets = {{"documents", "Documents", "/my-files/backups", {"/safe/documents"}, {}},
                   {"photos", "Photos", "/my-files/backups", {"/safe/photos"}, {}}};
    QVERIFY(BackupConfigStore(configPath).save(config));
    BackupRunStore runs(home.filePath("omacustos-backup-runs.json"));
    runs.ensureSet("documents");
    auto &record = *runs.find("documents");
    runs.markRunning(record);
    QVERIFY(runs.save());
    BackupEngine engine;
    BackupSetController controller(engine, configPath);
    controller.setCurrentIndex(1);
    QCOMPARE(controller.remainingTimes().value("documents").toString(), QString("Estimating time remaining…"));
    QVERIFY(!controller.remainingTimes().contains("photos"));
    record.progress = {4000, 1000, 4, 1, false};
    record.progressElapsedMs = 10000;
    record.progressUpdatedAt = QDateTime::currentDateTimeUtc();
    record.progress.verifiedFiles = 1;
    record.progress.failedItems = 1;
    record.progress.currentFile = "/safe/large file";
    record.progress.currentFileBytes = 3000;
    record.progress.phase = "uploading";
    QVERIFY(runs.save());
    controller.refreshRunState();
    QVERIFY(controller.remainingTimes().value("documents").toString().startsWith("Est. remaining: 00:"));
    const auto transfer = controller.transferProgress().value("documents").toMap();
    QCOMPARE(transfer.value("fraction").toDouble(), 0.25);
    QVERIFY(transfer.value("text").toString().contains("1 of 4 files processed · 1 verified · 1 items failed"));
    QVERIFY(transfer.value("text").toString().contains("Uploading: /safe/large file"));
    QVERIFY(!controller.transferProgress().contains("photos"));
    for (const QString &phase : {QString("staging"), QString("uploading-folder")}) {
        record.progress.phase = phase;
        record.progress.processedFiles = 0;
        record.progress.processedBytes = 0;
        QVERIFY(runs.save());
        controller.refreshRunState();
        const auto folderProgress = controller.transferProgress().value("documents").toMap();
        QVERIFY(folderProgress.value("indeterminate").toBool());
        QVERIFY(folderProgress.value("text").toString().contains(phase == "staging"
            ? "Preparing backup folder:" : "Uploading folder:"));
        QVERIFY(controller.remainingTimes().value("documents").toString() != "Finalizing backup…");
    }
    record.progress.finalizing = true;
    QVERIFY(runs.save());
    controller.refreshRunState();
    QCOMPARE(controller.remainingTimes().value("documents").toString(), QString("Finalizing backup…"));
    runs.markSuccess(record, QDateTime::currentDateTimeUtc());
    QVERIFY(runs.save());
    controller.refreshRunState();
    QVERIFY(controller.remainingTimes().isEmpty());
    QVERIFY(controller.transferProgress().isEmpty());
}

void BackupSetControllerTest::reportsVerifiedCountsAndFailureDetailsWithoutInventingLegacyCounts()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString configPath = home.filePath("settings.json");
    BackupConfig config;
    config.sets = {{"documents", "Documents", "/my-files/backups", {"/safe/documents"}, {}}};
    QVERIFY(BackupConfigStore(configPath).save(config));
    BackupRunStore runs(home.filePath("omacustos-backup-runs.json"));
    runs.ensureSet("documents");
    auto &record = *runs.find("documents");
    runs.markIncomplete(record, "Backup incomplete", QDateTime::currentDateTimeUtc());
    record.result = {true, true, 97, 1000, {{"/safe/a", "uploading", "Connection interrupted"},
        {"/safe/b", "uploading", "Connection interrupted"}, {"/safe/c", "reading", "Permission denied"}}};
    QVERIFY(runs.save());
    BackupEngine engine;
    BackupSetController controller(engine, configPath);
    QCOMPARE(controller.recentBackups().first(), QString("Documents\nIncomplete"));
    auto details = controller.runDetails().value("documents").toMap();
    QCOMPARE(details.value("summary").toString(), QString("97 files backed up · 3 items failed"));
    const auto issues = details.value("issues").toList();
    QCOMPARE(issues.size(), 3);
    QCOMPARE(issues.last().toMap().value("path").toString(), QString("/safe/c"));
    QCOMPARE(issues.last().toMap().value("reason").toString(), QString("Permission denied"));
    record.result.manifestVerified = false;
    runs.markFailed(record, "Unable to upload manifest", QDateTime::currentDateTimeUtc());
    QVERIFY(runs.save());
    controller.refreshRunState();
    details = controller.backupDetails("documents");
    QCOMPARE(details.value("status").toString(), QString("Failed"));
    QVERIFY(details.value("summary").toString().isEmpty());
    QCOMPARE(details.value("error").toString(), QString("Unable to upload manifest"));
    record.result = {};
    record.status = "success";
    record.lastError.clear();
    QVERIFY(runs.save());
    controller.refreshRunState();
    QVERIFY(controller.backupDetails("documents").value("summary").toString().isEmpty());
    QCOMPARE(controller.recentBackups().first(), QString("Documents\nSuccessful"));
}

void BackupSetControllerTest::successfulSaveNotifiesSchedulingButPreviewAndFailedSaveDoNot()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    BackupEngine engine;
    BackupSetController controller(engine, directory.filePath("settings.json"));
    QSignalSpy saved(&controller, &BackupSetController::configurationSaved);
    controller.addSet();
    QVariantMap draft = currentDraft(controller);
    draft["scheduleFrequency"] = "daily";
    controller.applyCurrentDraft(draft);
    controller.preview();
    QVERIFY(saved.isEmpty());
    QVERIFY(!controller.save());
    QVERIFY(saved.isEmpty());
    draft["sources"] = QStringList {"/safe/documents"};
    controller.applyCurrentDraft(draft);
    QVERIFY(controller.save());
    QCOMPARE(saved.count(), 1);
    controller.removeCurrentSet();
    QCOMPARE(saved.count(), 2);
}

void BackupSetControllerTest::discardingUnsavedSetDoesNotWriteConfiguration_data()
{
    QTest::addColumn<bool>("existingSet");
    QTest::addColumn<bool>("failedSave");
    QTest::newRow("first set") << false << false;
    QTest::newRow("existing sets") << true << false;
    QTest::newRow("failed first save") << false << true;
    QTest::newRow("failed save with existing sets") << true << true;
}

void BackupSetControllerTest::discardingUnsavedSetDoesNotWriteConfiguration()
{
    QFETCH(bool, existingSet);
    QFETCH(bool, failedSave);
    QTemporaryDir directory;
    const QString settings = directory.filePath("settings.json");
    QByteArray original;
    if (existingSet) {
        BackupConfig config;
        config.sets = {{"documents", "Documents", "/backups", {"/safe/documents"}, {}}};
        QVERIFY(BackupConfigStore(settings).save(config));
        QFile file(settings);
        QVERIFY(file.open(QIODevice::ReadOnly));
        original = file.readAll();
    }
    BackupEngine engine;
    BackupSetController controller(engine, settings);
    QSignalSpy saved(&controller, &BackupSetController::configurationSaved);
    controller.addSet();
    const QString draftId = controller.currentId();
    QVariantMap draft = currentDraft(controller);
    draft["scheduleFrequency"] = "daily";
    draft["sources"] = QStringList {"/safe/draft"};
    controller.applyCurrentDraft(draft);
    controller.preview();
    QTRY_VERIFY(controller.previewAvailable());
    if (failedSave) {
        draft["name"] = "";
        controller.applyCurrentDraft(draft);
        QVERIFY(!controller.save());
    }
    controller.discardUnsavedSet();
    QVERIFY(!controller.setIds().contains(draftId));
    QCOMPARE(controller.setNames(), existingSet ? QStringList {"Documents"} : QStringList {});
    QCOMPARE(controller.currentIndex(), existingSet ? 0 : -1);
    QVERIFY(!controller.previewAvailable());
    QVERIFY(!controller.previewBusy());
    QVERIFY(saved.isEmpty());
    QCOMPARE(QFile::exists(settings), existingSet);
    if (existingSet) {
        QFile file(settings);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original);
    }
    controller.discardUnsavedSet();
    QCOMPARE(controller.setNames(), existingSet ? QStringList {"Documents"} : QStringList {});
}

void BackupSetControllerTest::savedSetIsKeptWhenEditorIsCancelled()
{
    QTemporaryDir directory;
    const QString settings = directory.filePath("settings.json");
    BackupEngine engine;
    BackupSetController controller(engine, settings);
    controller.addSet();
    controller.setCurrentName("Saved documents");
    controller.setCurrentSources({"/safe/documents"});
    const QString id = controller.currentId();
    QVERIFY(controller.save());
    controller.discardUnsavedSet();
    QCOMPARE(controller.setIds(), QStringList {id});
    BackupSetController reopened(engine, settings);
    QCOMPARE(reopened.setIds(), QStringList {id});
    QCOMPARE(reopened.setNames(), QStringList {"Saved documents"});
}

void BackupSetControllerTest::mergingImportDoesNotSaveAnUnfinishedNewSet()
{
    QTemporaryDir directory;
    const QString settings = directory.filePath("settings.json");
    const QString importedPath = directory.filePath("import.json");
    BackupConfig imported;
    imported.sets = {{"documents", "Documents", "/backups", {"/safe/documents"}, {}}};
    QVERIFY(BackupConfigStore(importedPath).exportSets(imported));
    BackupEngine engine;
    BackupSetController controller(engine, settings);
    controller.addSet();
    controller.setCurrentSources({"/safe/draft"});
    QVERIFY(controller.importSets(importedPath, true));
    controller.discardUnsavedSet();
    QCOMPARE(controller.setIds(), QStringList {"documents"});
    BackupSetController reopened(engine, settings);
    QCOMPARE(reopened.setIds(), QStringList {"documents"});
}

void BackupSetControllerTest::previewUpdatesFilesWithoutCountMessage_data()
{
    QTest::addColumn<bool>("hasSource");
    QTest::newRow("empty selection") << false;
    QTest::newRow("selected file") << true;
}

void BackupSetControllerTest::previewUpdatesFilesWithoutCountMessage()
{
    QFETCH(bool, hasSource);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile source(directory.filePath(QStringLiteral("notes.txt")));
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("important content");
    source.close();

    BackupEngine engine;
    BackupSetController controller(engine, directory.filePath(QStringLiteral("settings.json")));
    controller.addSet();
    if (hasSource) {
        controller.setCurrentSources({source.fileName()});
    }
    QSignalSpy previewSpy(&controller, &BackupSetController::previewChanged);
    QSignalSpy statusSpy(&controller, &BackupSetController::statusChanged);
    QSignalSpy failureSpy(&controller, &BackupSetController::failed);

    QVERIFY(!controller.previewAvailable());
    controller.preview();

    QTRY_VERIFY(controller.previewAvailable());
    QCOMPARE(previewSpy.count(), 1);
    QCOMPARE(controller.previewIncluded(), hasSource ? QStringList {source.fileName()} : QStringList {});
    QCOMPARE(statusSpy.count(), 1);
    QVERIFY(statusSpy.first().first().toString().isEmpty());
    QVERIFY(failureSpy.isEmpty());
}

void BackupSetControllerTest::previewGroupsAreReadOnlyAndResetWithSelection_data()
{
    QTest::addColumn<bool>("mixed");
    QTest::newRow("mixed results") << true;
    QTest::newRow("only missing and skipped") << false;
}

void BackupSetControllerTest::previewGroupsAreReadOnlyAndResetWithSelection()
{
    QFETCH(bool, mixed);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString included = directory.filePath("notes.txt");
    const QString excluded = directory.filePath("excluded.txt");
    const QString skipped = directory.filePath("link.txt");
    const QString missing = directory.filePath("missing.txt");
    for (const QString &path : {included, excluded}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("content"), 7);
    }
    QVERIFY(QFile::link(included, skipped));
    const QString configPath = directory.filePath("settings.json");
    BackupConfig config;
    config.sets = {{"documents", "Documents", "/my-files/backups", {included}, {}},
                   {"photos", "Photos", "/my-files/backups", {included}, {}}};
    QVERIFY(BackupConfigStore(configPath).save(config));
    QFile savedConfig(configPath);
    QVERIFY(savedConfig.open(QIODevice::ReadOnly));
    const QByteArray originalConfig = savedConfig.readAll();
    savedConfig.close();

    BackupEngine engine;
    BackupSetController controller(engine, configPath);
    QVariantMap draft = currentDraft(controller);
    draft["sources"] = mixed ? QStringList {included, excluded, skipped, missing} : QStringList {skipped, missing};
    draft["exclusions"] = QStringList {excluded};
    draft["scheduleFrequency"] = "daily";
    controller.applyCurrentDraft(draft);
    QSignalSpy saved(&controller, &BackupSetController::configurationSaved);
    controller.preview();

    QTRY_VERIFY(controller.previewAvailable());
    QCOMPARE(controller.previewIncluded(), mixed ? QStringList {included} : QStringList {});
    QCOMPARE(controller.previewExcluded(), mixed ? QStringList {excluded} : QStringList {});
    QCOMPARE(controller.previewSkipped(), QStringList {skipped});
    QCOMPARE(controller.previewMissing(), QStringList {missing});
    QVERIFY(saved.isEmpty());
    QVERIFY(controller.runningSetIds().isEmpty());
    QVERIFY(!QFile::exists(directory.filePath("omacustos-backup-runs.json")));
    QVERIFY(savedConfig.open(QIODevice::ReadOnly));
    QCOMPARE(savedConfig.readAll(), originalConfig);

    controller.setCurrentIndex(1);
    QVERIFY(!controller.previewAvailable());
    QVERIFY(controller.previewIncluded().isEmpty());
    QVERIFY(controller.previewExcluded().isEmpty());
    QVERIFY(controller.previewSkipped().isEmpty());
    QVERIFY(controller.previewMissing().isEmpty());
    controller.preview();
    QTRY_VERIFY(controller.previewAvailable());
    QCOMPARE(controller.previewIncluded(), QStringList {included});
    controller.addSet();
    QVERIFY(!controller.previewAvailable());
}

void BackupSetControllerTest::recentBackupTimestampIncludesLocalDateAndSeconds()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString configPath = directory.filePath(QStringLiteral("settings.json"));
    BackupConfig config;
    config.sets = {{QStringLiteral("documents-id"), QStringLiteral("Documents"),
        QStringLiteral("/my-files/backups"), {QStringLiteral("/safe/documents")}, {}}};
    QVERIFY(BackupConfigStore(configPath).save(config));
    BackupRunStore runs(directory.filePath(QStringLiteral("omacustos-backup-runs.json")));
    runs.ensureSet(QStringLiteral("documents-id"));
    runs.markSuccess(*runs.find(QStringLiteral("documents-id")), QDateTime(QDate(2026, 10, 2), QTime(9, 30, 45)));
    QVERIFY(runs.save());

    BackupEngine engine;
    BackupSetController controller(engine, configPath);
    QCOMPARE(controller.recentBackupTimestamps(), QStringList {QStringLiteral("02/10/2026 09:30:45")});
}

void BackupSetControllerTest::folderPathUsesBackupIdentityAndWorkerNaming()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    BackupEngine engine;
    BackupSetController controller(engine, directory.filePath(QStringLiteral("settings.json")));
    controller.addSet();
    controller.setCurrentName(QStringLiteral("Documents / notes"));
    controller.setCurrentRemoteRoot(QStringLiteral("/my-files/custom-backups"));
    const QString documentsId = controller.currentId();
    controller.addSet();
    controller.setCurrentName(QStringLiteral("Photos"));

    QString hostname = QSysInfo::machineHostName();
    hostname.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}._-]")), QStringLiteral("_"));
    const QString expected = QStringLiteral("/my-files/custom-backups/%1/Documents___notes").arg(hostname);
    QCOMPARE(controller.recentBackupFolderPath(documentsId), expected);
    QVERIFY(controller.recentBackupFolderPath(QStringLiteral("removed-id")).isEmpty());

    BackupRunStore runs(directory.filePath(QStringLiteral("omacustos-backup-runs.json")));
    runs.ensureSet(documentsId);
    runs.find(documentsId)->remoteCopyPath = QStringLiteral("/my-files/backups/previous-computer/Original_name/copy-id");
    QVERIFY(runs.save());
    controller.refreshRunState();
    QCOMPARE(controller.recentBackupFolderPath(documentsId), QStringLiteral("/my-files/backups/previous-computer/Original_name"));
}

void BackupSetControllerTest::deletedCopyDisappearsFromRecentBackupsButKeepsItsSet()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString configPath = directory.filePath(QStringLiteral("settings.json"));
    BackupConfig config;
    config.sets = {{QStringLiteral("documents-id"), QStringLiteral("Documents"),
        QStringLiteral("/my-files/backups"), {QStringLiteral("/safe/documents")}, {}}};
    QVERIFY(BackupConfigStore(configPath).save(config));
    BackupRunStore runs(directory.filePath(QStringLiteral("omacustos-backup-runs.json")));
    runs.ensureSet(QStringLiteral("documents-id"));
    runs.find(QStringLiteral("documents-id"))->status = QStringLiteral("copy_deleted");
    QVERIFY(runs.save());
    BackupEngine engine;
    BackupSetController controller(engine, configPath);
    QCOMPARE(controller.setNames(), QStringList {QStringLiteral("Documents")});
    QVERIFY(controller.recentBackups().isEmpty());
    QVERIFY(controller.recentBackupSetIds().isEmpty());
    QVERIFY(controller.recentBackupTimestamps().isEmpty());
}

void BackupSetControllerTest::exportsSavedSetsAndImportsTheirSettings()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString originalPath = directory.filePath(QStringLiteral("original/settings.json"));
    BackupConfig original;
    original.protonBinary = QStringLiteral("/old-machine/proton-drive");
    BackupSet projects {QStringLiteral("projects-id"), QStringLiteral("Projects"), QStringLiteral("/my-files/projects"),
        {QStringLiteral("/home/user/projects"), QStringLiteral("/home/user/notes.txt")},
        {QStringLiteral("node_modules"), QStringLiteral("/home/user/projects/cache")}};
    projects.schedule = {QStringLiteral("monthly"), 9, 30, 2, 31};
    projects.retention = 7;
    projects.onlyOnAcPower = true;
    original.sets = {projects, {QStringLiteral("photos-id"), QStringLiteral("Photos"),
        QStringLiteral("/my-files/backups"), {QStringLiteral("/home/user/photos")}, {}}};
    QVERIFY(BackupConfigStore(originalPath).save(original));
    BackupEngine engine;
    BackupSetController source(engine, originalPath);
    source.setCurrentName(QStringLiteral("Unsaved draft"));
    const QString exportPath = directory.filePath(QStringLiteral("backup sets.json"));
    QVERIFY(source.exportSets(exportPath));
    QFile exported(exportPath);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    const QJsonObject document = QJsonDocument::fromJson(exported.readAll()).object();
    QCOMPARE(document.value(QStringLiteral("application")).toString(), QStringLiteral("omacustos"));
    QVERIFY(!document.contains(QStringLiteral("proton_binary")));
    QVERIFY(!document.contains(QStringLiteral("runs")));

    const QString destinationPath = directory.filePath(QStringLiteral("destination/settings.json"));
    BackupConfig previous;
    previous.protonBinary = QStringLiteral("/this-machine/proton-drive");
    previous.sets = {{QStringLiteral("previous-id"), QStringLiteral("Previous"),
        QStringLiteral("/my-files/backups"), {QStringLiteral("/safe/previous")}, {}}};
    QVERIFY(BackupConfigStore(destinationPath).save(previous));
    BackupSetController destination(engine, destinationPath);
    QSignalSpy changed(&destination, &BackupSetController::setsChanged);
    QVERIFY(destination.importSets(exportPath));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(destination.setNames(), (QStringList {QStringLiteral("Projects"), QStringLiteral("Photos")}));
    QCOMPARE(destination.currentId(), projects.id);
    QCOMPARE(destination.currentSources(), projects.sourceDirectories);
    QCOMPARE(destination.currentExclusions(), projects.exclusions);
    QCOMPARE(destination.currentScheduleFrequency(), QStringLiteral("monthly"));
    QCOMPARE(destination.currentScheduleHour(), 9);
    QCOMPARE(destination.currentScheduleMinute(), 30);
    QCOMPARE(destination.currentScheduleDayOfMonth(), 31);
    QCOMPARE(destination.currentRetention(), 7);
    QVERIFY(destination.currentOnlyOnAcPower());
    BackupConfig restored;
    QVERIFY(BackupConfigStore(destinationPath).load(&restored));
    QCOMPARE(restored.protonBinary, previous.protonBinary);
    QCOMPARE(restored.sets.size(), 2);
    QCOMPARE(restored.sets.first().remoteRoot, projects.remoteRoot);
}

void BackupSetControllerTest::invalidImportLeavesExistingSetsUntouched_data()
{
    QTest::addColumn<QByteArray>("contents");
    QTest::addColumn<QString>("errorField");
    QTest::addColumn<bool>("merge");
    const auto addRow = [](const char *name, const QByteArray &contents, const QString &field) {
        QTest::newRow(qPrintable(QStringLiteral("%1 replace").arg(name))) << contents << field << false;
        QTest::newRow(qPrintable(QStringLiteral("%1 merge").arg(name))) << contents << field << true;
    };
    addRow("invalid JSON", "{", "invalid JSON at byte");
    addRow("not an object", "[]", "JSON object");
    addRow("not an export", R"({"sets":[]})", "application");
    addRow("unknown version", R"({"application":"omacustos","version":2,"sets":[]})", "version");
    addRow("boolean version", R"({"application":"omacustos","version":true,"sets":[]})", "version");
    addRow("wrong sets type", R"({"application":"omacustos","version":1,"sets":{}})", "sets");
    addRow("wrong set type", R"({"application":"omacustos","version":1,"sets":[null]})", "sets[0]");
    addRow("invalid sources", R"({"application":"omacustos","version":1,"sets":[{"id":"a","name":"A","remote_root":"/my-files/backups","source_directories":[]}]})", "sets[0].source_directories");
    addRow("duplicate identities", R"({"application":"omacustos","version":1,"sets":[{"id":"a","name":"A","remote_root":"/my-files/backups","source_directories":["/safe/a"]},{"id":"a","name":"B","remote_root":"/my-files/backups","source_directories":["/safe/b"]}]})", "sets[1].id");

    const QJsonObject validSet = QJsonDocument::fromJson(R"({"id":"a","name":"A","remote_root":"/my-files/backups","source_directories":["/safe/a"]})").object();
    const auto addField = [&](const char *name, const QString &field, const QJsonValue &value) {
        QJsonObject set = validSet;
        set.insert(field, value);
        const QJsonObject document {{"application", "omacustos"}, {"version", 1}, {"sets", QJsonArray {set}}};
        addRow(name, QJsonDocument(document).toJson(), "sets[0]." + field);
    };
    addField("blank id", "id", " ");
    addField("wrong name type", "name", 12);
    addField("blank remote root", "remote_root", "");
    addField("wrong sources type", "source_directories", "/safe/a");
    addField("blank source", "source_directories", QJsonArray {" "});
    addField("wrong source item type", "source_directories", QJsonArray {42});
    addField("wrong exclusions type", "exclusions", "node_modules");
    addField("wrong exclusion item type", "exclusions", QJsonArray {false});
    addField("wrong schedule type", "schedule", QJsonValue::Null);
    addField("zero retention", "retention", 0);
    addField("string retention", "retention", "3");
    addField("fractional retention", "retention", 3.5);
    addField("overflow retention", "retention", 2147483648.0);
    addField("wrong AC power type", "only_on_ac_power", "false");

    const auto addSchedule = [&](const char *name, const QString &field, const QJsonValue &value) {
        QJsonObject set = validSet;
        set.insert("schedule", QJsonObject {{field, value}});
        const QJsonObject document {{"application", "omacustos"}, {"version", 1}, {"sets", QJsonArray {set}}};
        addRow(name, QJsonDocument(document).toJson(), "sets[0].schedule." + field);
    };
    addSchedule("unknown frequency", "frequency", "hourly");
    addSchedule("wrong frequency type", "frequency", false);
    addSchedule("wrong hour type", "hour", "2");
    addSchedule("boolean hour", "hour", true);
    addSchedule("fractional hour", "hour", 2.5);
    addSchedule("negative hour", "hour", -1);
    addSchedule("hour out of range", "hour", 24);
    addSchedule("minute out of range", "minute", 60);
    addSchedule("weekday out of range", "weekday", 0);
    addSchedule("monthly day out of range", "day_of_month", 32);
}

void BackupSetControllerTest::invalidImportLeavesExistingSetsUntouched()
{
    QFETCH(QByteArray, contents);
    QFETCH(QString, errorField);
    QFETCH(bool, merge);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString configPath = directory.filePath(QStringLiteral("settings.json"));
    BackupConfig config;
    config.sets = {{QStringLiteral("documents-id"), QStringLiteral("Documents"),
        QStringLiteral("/my-files/backups"), {QStringLiteral("/safe/documents")}, {}}};
    QVERIFY(BackupConfigStore(configPath).save(config));
    QFile saved(configPath);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    const QByteArray before = saved.readAll();
    saved.close();
    QFile invalid(directory.filePath(QStringLiteral("invalid.json")));
    QVERIFY(invalid.open(QIODevice::WriteOnly));
    invalid.write(contents);
    invalid.close();
    BackupEngine engine;
    BackupSetController controller(engine, configPath);
    QSignalSpy failure(&controller, &BackupSetController::failed);
    QSignalSpy changed(&controller, &BackupSetController::setsChanged);
    QSignalSpy savedConfiguration(&controller, &BackupSetController::configurationSaved);
    QVERIFY(!controller.importSets(invalid.fileName(), merge));
    QCOMPARE(failure.count(), 1);
    QVERIFY2(failure.first().first().toString().contains(errorField), qPrintable(failure.first().first().toString()));
    QVERIFY(changed.isEmpty());
    QVERIFY(savedConfiguration.isEmpty());
    QCOMPARE(controller.setNames(), QStringList {QStringLiteral("Documents")});
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QCOMPARE(saved.readAll(), before);
}

void BackupSetControllerTest::importsAnnotatedTemplate()
{
    const QString templatePath = QFINDTESTDATA("../../backup-sets.template.json");
    QVERIFY(!templatePath.isEmpty());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString configPath = directory.filePath("settings.json");
    BackupEngine engine;
    BackupSetController controller(engine, configPath);
    QVERIFY(controller.importSets(templatePath));
    QCOMPARE(controller.setNames(), QStringList {"Personal files"});
    QCOMPARE(controller.currentSources().size(), 3);
    QCOMPARE(controller.currentScheduleFrequency(), QString("daily"));
    QCOMPARE(controller.currentScheduleHour(), 2);
    QCOMPARE(controller.currentScheduleMinute(), 0);
    QCOMPARE(controller.currentRetention(), 3);
    QVERIFY(!controller.currentOnlyOnAcPower());
    QFile persisted(configPath);
    QVERIFY(persisted.open(QIODevice::ReadOnly));
    QVERIFY(!persisted.readAll().contains("_comment"));
}

void BackupSetControllerTest::savesBundledTemplateWithoutChangingConfiguration()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString configPath = directory.filePath("settings.json");
    QVERIFY(BackupConfigStore(configPath).save(BackupConfig {}));
    QFile configuration(configPath);
    QVERIFY(configuration.open(QIODevice::ReadOnly));
    const QByteArray before = configuration.readAll();
    configuration.close();
    BackupEngine engine;
    BackupSetController controller(engine, configPath);
    QVERIFY(controller.setNames().isEmpty());
    QSignalSpy saved(&controller, &BackupSetController::configurationSaved);
    QSignalSpy changed(&controller, &BackupSetController::setsChanged);
    QSignalSpy status(&controller, &BackupSetController::statusChanged);
    const QString outputPath = directory.filePath("downloaded template.json");
    QVERIFY(controller.saveTemplate(outputPath));
    QCOMPARE(status.count(), 1);
    QVERIFY(saved.isEmpty());
    QVERIFY(changed.isEmpty());
    QVERIFY(controller.setNames().isEmpty());
    QVERIFY(configuration.open(QIODevice::ReadOnly));
    QCOMPARE(configuration.readAll(), before);

    QFile downloaded(outputPath);
    QVERIFY(downloaded.open(QIODevice::ReadOnly));
    QFile original(QFINDTESTDATA("../../backup-sets.template.json"));
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(downloaded.readAll(), original.readAll());
    BackupConfig imported;
    QString error;
    QVERIFY2(BackupConfigStore(outputPath).importSets(&imported, &error), qPrintable(error));
    QCOMPARE(imported.sets.first().schedule.frequency, QString("daily"));
    QCOMPARE(imported.sets.first().schedule.hour, 2);
}

void BackupSetControllerTest::templateCannotOverwriteLocalStateOrReportFailedWritesAsSuccess()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString configPath = directory.filePath("settings.json");
    QVERIFY(BackupConfigStore(configPath).save(BackupConfig {}));
    BackupEngine engine;
    BackupSetController controller(engine, configPath);
    QSignalSpy failure(&controller, &BackupSetController::failed);
    QSignalSpy success(&controller, &BackupSetController::statusChanged);
    for (const QString &path : {configPath, directory.filePath("omacustos-backup-runs.json"),
        directory.filePath("omacustos-backup-cleanup.json"), directory.path(), directory.filePath("missing/template.json")}) {
        QVERIFY(!controller.saveTemplate(path));
    }
    QCOMPARE(failure.count(), 5);
    QVERIFY(success.isEmpty());
    BackupConfig reloaded;
    QVERIFY(BackupConfigStore(configPath).load(&reloaded));
    QVERIFY(reloaded.sets.isEmpty());
}

void BackupSetControllerTest::mergesSetsByIdAndPreservesOtherSets()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString configPath = directory.filePath("settings.json");
    BackupConfig original;
    original.protonBinary = "/this-machine/proton-drive";
    original.sets = {
        {"documents", "Documents", "/my-files/backups", {"/safe/documents"}, {}},
        {"photos", "Photos", "/my-files/backups", {"/safe/photos"}, {}},
    };
    QVERIFY(BackupConfigStore(configPath).save(original));
    // Omitted optional fields use defaults. Matching names do not merge distinct IDs.
    QFile imported(directory.filePath("import.json"));
    QVERIFY(imported.open(QIODevice::WriteOnly));
    imported.write(R"({"application":"omacustos","version":1,"sets":[
        {"id":"documents","name":"Updated documents","remote_root":"/my-files/new","source_directories":["/safe/new-documents"],"schedule":{"frequency":"daily","hour":2}},
        {"id":"new-photos","name":"Photos","remote_root":"/my-files/backups","source_directories":["/safe/new-photos"]}
    ]})");
    imported.close();
    BackupEngine engine;
    BackupSetController controller(engine, configPath);
    QSignalSpy saved(&controller, &BackupSetController::configurationSaved);
    QVERIFY(controller.importSets(imported.fileName(), true));
    QCOMPARE(saved.count(), 1);
    QCOMPARE(controller.setIds(), (QStringList {"documents", "photos", "new-photos"}));
    QCOMPARE(controller.setNames(), (QStringList {"Updated documents", "Photos", "Photos"}));
    BackupConfig reloaded;
    QVERIFY(BackupConfigStore(configPath).load(&reloaded));
    QCOMPARE(reloaded.protonBinary, original.protonBinary);
    QCOMPARE(reloaded.sets.at(0).sourceDirectories, QStringList {"/safe/new-documents"});
    QCOMPARE(reloaded.sets.at(0).remoteRoot, QString("/my-files/new"));
    QCOMPARE(reloaded.sets.at(0).schedule.frequency, QString("daily"));
    QCOMPARE(reloaded.sets.at(0).schedule.hour, 2);
    QCOMPARE(reloaded.sets.at(1).sourceDirectories, original.sets.at(1).sourceDirectories);
    QCOMPARE(reloaded.sets.at(2).schedule.frequency, QString("disabled"));
    QCOMPARE(reloaded.sets.at(2).retention, 3);
    QVERIFY(reloaded.sets.at(2).exclusions.isEmpty());
    QVERIFY(!reloaded.sets.at(2).onlyOnAcPower);
}

void BackupSetControllerTest::emptyMergeKeepsExistingSets()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString configPath = directory.filePath("settings.json");
    BackupConfig original;
    original.sets = {{"documents", "Documents", "/my-files/backups", {"/safe/documents"}, {}}};
    QVERIFY(BackupConfigStore(configPath).save(original));
    const QString importPath = directory.filePath("empty.json");
    QVERIFY(BackupConfigStore(importPath).exportSets(BackupConfig {}));
    BackupEngine engine;
    BackupSetController controller(engine, configPath);
    QVERIFY(controller.importSets(importPath, true));
    QCOMPARE(controller.setIds(), QStringList {"documents"});
    BackupConfig reloaded;
    QVERIFY(BackupConfigStore(configPath).load(&reloaded));
    QCOMPARE(reloaded.sets.size(), 1);
    QCOMPARE(reloaded.sets.first().sourceDirectories, original.sets.first().sourceDirectories);
}

void BackupSetControllerTest::summarizesIndependentCardSettingsWithoutChangingSelection()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString configPath = directory.filePath("settings.json");
    BackupConfig config;
    config.sets = {
        {"documents", "Documents", "/my-files/backups", {"/safe/documents", "/safe/notes.txt"}, {}},
        {"photos", "Photos", "/my-files/backups", {"/safe/photos"}, {}},
    };
    config.sets[0].schedule = {"daily", 2, 0, 1, 1};
    config.sets[1].retention = 7;
    config.sets[1].onlyOnAcPower = true;
    QVERIFY(BackupConfigStore(configPath).save(config));
    BackupEngine engine;
    BackupSetController controller(engine, configPath);
    const auto documents = controller.setSummaries().value("documents").toMap();
    const auto photos = controller.setSummaries().value("photos").toMap();
    QCOMPARE(documents.value("sourceCount").toInt(), 2);
    QCOMPARE(documents.value("schedule").toString(), QString("Daily at 02:00"));
    QVERIFY(!documents.value("nextRun").toString().isEmpty());
    QCOMPARE(photos.value("schedule").toString(), QString("Manual backups"));
    QVERIFY(photos.value("nextRun").toString().isEmpty());
    QCOMPARE(photos.value("retention").toInt(), 7);
    QVERIFY(photos.value("onlyOnAcPower").toBool());
    QCOMPARE(controller.currentIndex(), 0);
    controller.setCurrentSources({"/safe/only-one"});
    QCOMPARE(controller.setSummaries().value("documents").toMap().value("sourceCount").toInt(), 1);
    controller.setCurrentScheduleFrequency("disabled");
    QVERIFY(controller.setSummaries().value("documents").toMap().value("nextRun").toString().isEmpty());
    QCOMPARE(controller.setIds(), (QStringList {"documents", "photos"}));
}

void BackupSetControllerTest::retainsCopyNavigationAfterFailureAndLatestCopyDeletion()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString configPath = directory.filePath("settings.json");
    BackupConfig config;
    config.sets = {{"documents", "Documents", "/my-files/backups", {"/safe/documents"}, {}}};
    QVERIFY(BackupConfigStore(configPath).save(config));
    BackupRunStore runs(directory.filePath("omacustos-backup-runs.json"));
    runs.ensureSet("documents");
    auto &record = *runs.find("documents");
    runs.markSuccess(record, QDateTime::currentDateTimeUtc().addDays(-1));
    runs.markFailed(record, "Upload interrupted", QDateTime::currentDateTimeUtc());
    QVERIFY(runs.save());
    BackupEngine engine;
    BackupSetController controller(engine, configPath);
    auto summary = controller.runSummaries().value("documents").toMap();
    QCOMPARE(summary.value("statusCode").toString(), QString("failed"));
    QVERIFY(summary.value("hasActivity").toBool());
    QVERIFY(!summary.value("lastSuccess").toString().isEmpty());
    QVERIFY(summary.value("lastAttempt").toString() != summary.value("lastSuccess").toString());
    record.status = "copy_deleted";
    record.remoteCopyPath.clear();
    QVERIFY(runs.save());
    controller.refreshRunState();
    summary = controller.runSummaries().value("documents").toMap();
    QVERIFY(summary.value("hasActivity").toBool());
    QVERIFY(!summary.value("hasLatestCopy").toBool());
    QCOMPARE(summary.value("statusCode").toString(), QString("copy_deleted"));
    QCOMPARE(controller.setIds(), QStringList {"documents"});
    QVERIFY(!controller.recentBackupFolderPath("documents").isEmpty());
}

void BackupSetControllerTest::emptyImportDoesNotRestoreLegacySources()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString configPath = directory.filePath(QStringLiteral("settings.json"));
    QFile legacyFile(configPath);
    QVERIFY(legacyFile.open(QIODevice::WriteOnly));
    legacyFile.write(R"({"source_directory":"/safe/legacy","remote_root":"/my-files/backups","proton_binary":"/custom/proton-drive"})");
    legacyFile.close();
    const QString exportPath = directory.filePath(QStringLiteral("empty.json"));
    QVERIFY(BackupConfigStore(exportPath).exportSets(BackupConfig {}));
    BackupEngine engine;
    BackupSetController controller(engine, configPath);
    QVERIFY(controller.importSets(exportPath));
    QVERIFY(controller.setNames().isEmpty());
    BackupConfig reloaded;
    QVERIFY(BackupConfigStore(configPath).load(&reloaded));
    QVERIFY(reloaded.sets.isEmpty());
    QCOMPARE(reloaded.protonBinary, QStringLiteral("/custom/proton-drive"));
    BackupSetController reopened(engine, configPath);
    QVERIFY(reopened.setIds().isEmpty());
}

void BackupSetControllerTest::removingFinalSetPersistsEmptyConfiguration_data()
{
    QTest::addColumn<bool>("legacy");
    QTest::newRow("saved scheduled backup") << false;
    QTest::newRow("legacy single-source backup") << true;
}

void BackupSetControllerTest::removingFinalSetPersistsEmptyConfiguration()
{
    QFETCH(bool, legacy);
    QTemporaryDir directory;
    QTemporaryDir remote;
    QVERIFY(directory.isValid());
    QVERIFY(remote.isValid());
    const QString configPath = directory.filePath("settings.json");
    const QString source = directory.filePath("Documents");
    const QString copyPath = remote.filePath("computer/Documents/copy");
    QVERIFY(QDir().mkpath(copyPath));
    QFile payload(QDir(copyPath).filePath("notes.txt"));
    QVERIFY(payload.open(QIODevice::WriteOnly));
    const QByteArray contents("Existing remote backup contents");
    QCOMPARE(payload.write(contents), qint64(contents.size()));
    payload.close();

    BackupConfig config;
    config.protonBinary = "/custom/proton-drive";
    BackupConfigStore store(configPath);
    if (legacy) {
        QFile legacyFile(configPath);
        QVERIFY(legacyFile.open(QIODevice::WriteOnly));
        legacyFile.write(QJsonDocument(QJsonObject {
            {"source_directory", source}, {"remote_root", remote.path()}, {"proton_binary", config.protonBinary},
        }).toJson());
    } else {
        BackupSet set {"documents", "Documents", remote.path(), {source}, {}};
        set.schedule.frequency = "daily";
        config.sets = {set};
        QVERIFY(store.save(config));
    }

    BackupEngine engine;
    BackupSetController controller(engine, configPath);
    QCOMPARE(controller.setNames(), QStringList {legacy ? "Default backup" : "Documents"});
    QCOMPARE(controller.currentSources(), QStringList {source});
    QCOMPARE(controller.currentScheduleFrequency(), QString(legacy ? "disabled" : "daily"));
    QSignalSpy saved(&controller, &BackupSetController::configurationSaved);
    QSignalSpy failed(&controller, &BackupSetController::failed);
    controller.removeCurrentSet();
    QVERIFY(failed.isEmpty());
    QCOMPARE(saved.count(), 1);
    QVERIFY(controller.setNames().isEmpty());
    QCOMPARE(controller.currentIndex(), -1);

    QFile persisted(configPath);
    QVERIFY(persisted.open(QIODevice::ReadOnly));
    const QJsonObject document = QJsonDocument::fromJson(persisted.readAll()).object();
    QVERIFY(document.value("sets").isArray());
    QVERIFY(document.value("sets").toArray().isEmpty());
    QVERIFY(!document.contains("source_directory"));
    QVERIFY(!document.contains("remote_root"));
    BackupConfig reloaded;
    QVERIFY(store.load(&reloaded));
    QVERIFY(reloaded.sets.isEmpty());
    QCOMPARE(reloaded.protonBinary, config.protonBinary);

    BackupSetController reopened(engine, configPath);
    QVERIFY(reopened.setNames().isEmpty());
    QVERIFY(reopened.setIds().isEmpty());
    QVERIFY(reopened.currentSources().isEmpty());
    QVERIFY(reopened.recentBackups().isEmpty());
    QCOMPARE(reopened.currentIndex(), -1);
    QCOMPARE(reopened.currentScheduleFrequency(), QStringLiteral("disabled"));
    QCOMPARE(reopened.currentNextRun(), QStringLiteral("Not scheduled"));
    QVERIFY(payload.open(QIODevice::ReadOnly));
    QCOMPARE(payload.readAll(), contents);
    QVERIFY(QFileInfo(copyPath).isDir());
}

void BackupSetControllerTest::exportCannotOverwriteLocalState()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString configPath = directory.filePath(QStringLiteral("settings.json"));
    QVERIFY(BackupConfigStore(configPath).save(BackupConfig {}));
    BackupEngine engine;
    BackupSetController controller(engine, configPath);
    QVERIFY(!controller.exportSets(configPath));
    QVERIFY(!controller.exportSets(directory.filePath(QStringLiteral("omacustos-backup-runs.json"))));
    QVERIFY(!controller.exportSets(directory.path()));
    BackupConfig reloaded;
    QVERIFY(BackupConfigStore(configPath).load(&reloaded));
    QCOMPARE(reloaded.protonBinary, QStringLiteral("proton-drive"));
}

void BackupSetControllerTest::importIsBlockedWhileWorkerRuns()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString configPath = directory.filePath(QStringLiteral("settings.json"));
    QVERIFY(BackupConfigStore(configPath).save(BackupConfig {}));
    const QString exportPath = directory.filePath(QStringLiteral("sets.json"));
    QVERIFY(BackupConfigStore(exportPath).exportSets(BackupConfig {}));
    QLockFile lock(configPath + QStringLiteral(".worker.lock"));
    QVERIFY(lock.tryLock(0));
    BackupEngine engine;
    BackupSetController controller(engine, configPath);
    QSignalSpy failure(&controller, &BackupSetController::failed);
    QVERIFY(!controller.importSets(exportPath));
    QCOMPARE(failure.count(), 1);
}

QTEST_GUILESS_MAIN(BackupSetControllerTest)
#include "backupsetcontroller_test.moc"

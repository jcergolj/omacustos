#include <QTemporaryDir>
#include <QTest>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>

#include "../src/backupprerequisites.h"
#include "../src/backuprunstore.h"
#include "../src/backupschedule.h"
#include "../src/protonfolderlink.h"

class FakePrerequisiteProbe final : public BackupPrerequisiteProbe
{
public:
    bool ac = true;

    bool onAcPower() const override { return ac; }
};

class ReliabilityTest final : public QObject
{
    Q_OBJECT

private slots:
    void monthlySchedulesUseTheLastDay();
    void runStoreCoalescesAndPersistsRetries();
    void prerequisitesGateAcPowerOnlyWhenRequired();
    void remainingTimeCountsDownAndHandlesIncompleteProgress();
    void progressPersistsAndResetsForANewAttempt();
    void olderRunRecordsHaveNoMadeUpEstimate();
    void previousSuccessProvidesAnInitialSingleFileEstimate();
    void workerPersistsDistinctResultsAndFailureDetails_data();
    void workerPersistsDistinctResultsAndFailureDetails();
};

void ReliabilityTest::previousSuccessProvidesAnInitialSingleFileEstimate()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    BackupRunStore store(home.filePath("runs.json"));
    store.ensureSet("documents");
    auto &record = *store.find("documents");
    const QDateTime now(QDate(2026, 10, 2), QTime(12, 0));
    store.markRunning(record);
    record.progress = {1000, 1000, 1, 1, true};
    record.progressElapsedMs = 20000;
    store.markSuccess(record, now);
    QVERIFY(store.save());
    BackupRunStore reopened(home.filePath("runs.json"));
    QVERIFY(reopened.load());
    QVERIFY(reopened.enqueue("documents", "manual", now));
    auto &next = *reopened.find("documents");
    reopened.markRunning(next);
    next.progress = {2000, 0, 1, 0, false};
    next.progressUpdatedAt = now;
    QCOMPARE(next.estimatedRemainingSeconds(now), qint64(40));
    QCOMPARE(next.estimatedRemainingSeconds(now.addSecs(5)), qint64(35));
    next.progress = {2000, 1000, 2, 1, false};
    next.progressElapsedMs = 10000;
    QCOMPARE(next.estimatedRemainingSeconds(now), qint64(10));
}

void ReliabilityTest::remainingTimeCountsDownAndHandlesIncompleteProgress()
{
    const QDateTime now(QDate(2026, 10, 2), QTime(12, 0));
    BackupRunRecord record;
    record.status = "running";
    record.progress = {1000, 200, 10, 2, false};
    record.progressElapsedMs = 20000;
    record.progressUpdatedAt = now;
    QCOMPARE(record.estimatedRemainingSeconds(now), qint64(80));
    QCOMPARE(record.estimatedRemainingSeconds(now.addSecs(5)), qint64(75));
    QCOMPARE(record.estimatedRemainingSeconds(now.addSecs(100)), qint64(0));
    QCOMPARE(record.estimatedRemainingSeconds(now.addSecs(-1)), qint64(-1));
    record.progress.totalBytes = 10000;
    QCOMPARE(record.estimatedRemainingSeconds(now), qint64(980));
    record.progress = {0, 0, 3, 1, false};
    QCOMPARE(record.estimatedRemainingSeconds(now), qint64(40));
    record.progress.totalBytes = 100;
    QCOMPARE(record.estimatedRemainingSeconds(now), qint64(-1));
    record.progress = {1000, 200, 10, 0, false};
    QCOMPARE(record.estimatedRemainingSeconds(now), qint64(-1));
    record.progress = {1000, 1000, 10, 10, true};
    QCOMPARE(record.estimatedRemainingSeconds(now), qint64(-1));
    record.progress = {1000, 200, 10, 2, false};
    record.status = "success";
    QCOMPARE(record.estimatedRemainingSeconds(now), qint64(-1));
}

void ReliabilityTest::progressPersistsAndResetsForANewAttempt()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    BackupRunStore store(home.filePath("runs.json"));
    store.ensureSet("documents");
    auto &record = *store.find("documents");
    store.markRunning(record);
    record.progress = {1000, 200, 10, 2, false};
    record.progressElapsedMs = 20000;
    record.progressUpdatedAt = QDateTime::currentDateTimeUtc();
    record.progress.verifiedFiles = 1;
    record.progress.verifiedBytes = 100;
    record.progress.failedItems = 1;
    record.progress.phase = "uploading";
    record.progress.currentFile = "/safe/large file";
    record.progress.currentFileBytes = 500;
    record.result = {true, true, 1, 100, {{"/safe/bad", "reading", "Permission denied"}}};
    QVERIFY(store.save());
    BackupRunStore reopened(home.filePath("runs.json"));
    QVERIFY(reopened.load());
    auto &restored = *reopened.find("documents");
    QCOMPARE(restored.progress.totalBytes, qint64(1000));
    QCOMPARE(restored.progress.processedBytes, qint64(200));
    QCOMPARE(restored.progress.totalFiles, 10);
    QCOMPARE(restored.progress.processedFiles, 2);
    QCOMPARE(restored.progressUpdatedAt, record.progressUpdatedAt);
    QCOMPARE(restored.progress.currentFile, record.progress.currentFile);
    QCOMPARE(restored.progress.phase, record.progress.phase);
    QCOMPARE(restored.progress.currentFileBytes, qint64(500));
    QCOMPARE(restored.progress.verifiedFiles, 1);
    QCOMPARE(restored.progress.verifiedBytes, qint64(100));
    QCOMPARE(restored.progress.failedItems, 1);
    QVERIFY(restored.result.manifestVerified);
    QCOMPARE(restored.result.issues.first().reason, QString("Permission denied"));
    QCOMPARE(restored.estimatedRemainingSeconds(record.progressUpdatedAt), qint64(80));
    reopened.markRunning(restored);
    QCOMPARE(restored.progress.totalFiles, 0);
    QVERIFY(!restored.result.reported);
    QVERIFY(restored.result.issues.isEmpty());
    QCOMPARE(restored.progressElapsedMs, qint64(0));
    QVERIFY(!restored.progressUpdatedAt.isValid());
}

void ReliabilityTest::workerPersistsDistinctResultsAndFailureDetails_data()
{
    QTest::addColumn<QString>("failure");
    QTest::addColumn<QString>("status");
    QTest::addColumn<int>("verified");
    QTest::addColumn<bool>("folderSource");
    for (bool folderSource : {false, true}) {
        const QString prefix = folderSource ? "folder: " : "files: ";
        QTest::newRow(qPrintable(prefix + "successful")) << QString("none") << QString("success") << 2 << folderSource;
        QTest::newRow(qPrintable(prefix + "partial upload")) << QString("partial") << QString("retrying") << 1 << folderSource;
        QTest::newRow(qPrintable(prefix + "all uploads failed")) << QString("all") << QString("retrying") << 0 << folderSource;
        QTest::newRow(qPrintable(prefix + "manifest upload failed")) << QString("manifest") << QString("retrying") << 2 << folderSource;
    }
}

void ReliabilityTest::workerPersistsDistinctResultsAndFailureDetails()
{
    QFETCH(QString, failure);
    QFETCH(QString, status);
    QFETCH(int, verified);
    QFETCH(bool, folderSource);
    QTemporaryDir home;
    QVERIFY(home.isValid());
    QFile cli(home.filePath("fake-proton"));
    QVERIFY(cli.open(QIODevice::WriteOnly));
    cli.write(R"CLI(#!/bin/bash
set -eu
case "$2" in
  list) printf '[]' ;;
  upload)
    source="${@: -2:1}"
    parent="${@: -1}"
    name="$(basename "$source")"
    if [[ "$FAILURE" == all && "$name" != manifest.json ]] ||
       [[ "$FAILURE" == partial && "$name" == bad.txt ]] ||
       [[ "$FAILURE" == manifest && "$name" == manifest.json ]]; then
      printf 'Connection interrupted' >&2
      exit 1
    fi
    mkdir -p "$FAKE_REMOTE$parent"
    if [[ -d "$source" ]]; then
      mkdir -p "$FAKE_REMOTE$parent/$name"
      if [[ "$FAILURE" == partial ]]; then
        cp -f "$source/good.txt" "$FAKE_REMOTE$parent/$name/good.txt"
        printf 'Connection interrupted' >&2
        exit 1
      fi
      cp -fR "$source/." "$FAKE_REMOTE$parent/$name/"
    else
      cp "$source" "$FAKE_REMOTE$parent/$name"
    fi
    ;;
  info)
    path="$FAKE_REMOTE${@: -1}"
    if [[ "$FAILURE" == none || "$FAILURE" == partial ]] && [[ -d "$path" ]]; then
      printf '{"type":"folder","uid":"volume~copy-node","deprecatedShareId":"share"}'
      exit 0
    fi
    [[ -f "$path" ]] || exit 1
    checksum="$(sha256sum "$path")"
    printf '{"size":%s,"sha256":"%s"}' "$(stat -c %s "$path")" "${checksum%% *}"
    ;;
  *) exit 1 ;;
esac
)CLI");
    cli.close();
    QVERIFY(cli.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    QStringList sources;
    const QString sourceFolder = home.filePath("source");
    QVERIFY(QDir().mkpath(sourceFolder));
    for (const QString &name : {QString("good.txt"), QString("bad.txt")}) {
        QFile file(folderSource ? QDir(sourceFolder).filePath(name) : home.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("content");
        sources.append(file.fileName());
    }
    if (folderSource) sources = {sourceFolder};
    BackupConfig config;
    config.protonBinary = cli.fileName();
    config.sets = {{"documents", "Documents", "/my-files/backups", sources, {}}};
    const QString configPath = home.filePath("settings.json");
    QVERIFY(BackupConfigStore(configPath).save(config));
    BackupRunStore store(home.filePath("omacustos-backup-runs.json"));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QVERIFY(store.enqueue("documents", "manual", now));
    QVERIFY(store.save());
    QProcess worker;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("FAKE_REMOTE", home.filePath("remote"));
    environment.insert("FAILURE", failure);
    environment.insert("TMPDIR", home.path());
    worker.setProcessEnvironment(environment);
    worker.start(QStringLiteral(OMACUSTOS_WORKER_BINARY), {"--config", configPath});
    QVERIFY(worker.waitForFinished(10000));
    QCOMPARE(worker.exitStatus(), QProcess::NormalExit);
    QVERIFY2(worker.exitCode() == 0, worker.readAllStandardError().constData());
    QVERIFY(store.load());
    const auto &record = *store.find("documents");
    QCOMPARE(record.status, status);
    QCOMPARE(record.result.verifiedFiles, verified);
    QCOMPARE(record.result.manifestVerified, failure == "none");
    const QUrl browserUrl = ProtonFolderLink::cached(ProtonFolderLink::cachePath(configPath), record.remoteCopyPath);
    if (failure == "none") {
        QCOMPARE(browserUrl.path(), QStringLiteral("/share/folder/copy-node"));
    } else {
        QVERIFY(browserUrl.isEmpty());
    }
    if (status == "success") {
        QVERIFY(record.lastSuccess.isValid());
        QVERIFY(!record.nextAttempt.isValid());
    } else {
        QVERIFY(record.lastFailure.isValid());
        QVERIFY(record.nextAttempt.isValid());
        QCOMPARE(store.readyIndexes(record.nextAttempt).size(), 1);
    }
    if (failure != "none") {
        QVERIFY(record.unfinished);
        QVERIFY(record.result.interrupted);
        QCOMPARE(record.lastError, QString("Connection interrupted"));
    }
    QVERIFY(QDir(home.path()).entryList({"omacustos-backup-*"}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}

void ReliabilityTest::olderRunRecordsHaveNoMadeUpEstimate()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    QFile file(home.filePath("runs.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"runs":[{"set_id":"documents","status":"running"}]})");
    file.close();
    BackupRunStore store(file.fileName());
    QVERIFY(store.load());
    const auto *record = store.find("documents");
    QVERIFY(record != nullptr);
    QCOMPARE(record->estimatedRemainingSeconds(QDateTime::currentDateTimeUtc()), qint64(-1));
}

void ReliabilityTest::monthlySchedulesUseTheLastDay()
{
    const BackupSchedule schedule {
        QStringLiteral("monthly"),
        9,
        30,
        1,
        31,
    };
    const QDateTime after(QDate(2026, 1, 31), QTime(10, 0));
    const QDateTime next = BackupScheduleCalculator::nextRun(schedule, after);

    QCOMPARE(next.date(), QDate(2026, 2, 28));
    QCOMPARE(next.time(), QTime(9, 30));
}

void ReliabilityTest::runStoreCoalescesAndPersistsRetries()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("runs.json"));
    const QDateTime now(QDate(2026, 1, 1), QTime(12, 0));
    BackupRunStore store(path);
    QVERIFY(store.load());
    QVERIFY(store.enqueue(QStringLiteral("set-a"), QStringLiteral("schedule"), now));
    QVERIFY(!store.enqueue(QStringLiteral("set-a"), QStringLiteral("schedule"), now));
    QCOMPARE(store.readyIndexes(now).size(), 1);

    BackupRunRecord *record = store.find(QStringLiteral("set-a"));
    QVERIFY(record != nullptr);
    store.markRunning(*record);
    store.markRetrying(*record, QStringLiteral("offline"), now);
    QVERIFY(store.save());

    BackupRunStore restored(path);
    QVERIFY(restored.load());
    const BackupRunRecord *restoredRecord = restored.find(QStringLiteral("set-a"));
    QVERIFY(restoredRecord != nullptr);
    QCOMPARE(restoredRecord->status, QStringLiteral("retrying"));
    QCOMPARE(restoredRecord->attempts, 1);
    QCOMPARE(restoredRecord->nextAttempt, now.addSecs(5));
}

void ReliabilityTest::prerequisitesGateAcPowerOnlyWhenRequired()
{
    BackupSet set;
    set.onlyOnAcPower = true;
    FakePrerequisiteProbe probe;

    probe.ac = false;
    BackupPrerequisiteResult result = BackupPrerequisites::check(set, probe);
    QVERIFY(!result.ready);
    QCOMPARE(result.reason, QStringLiteral("Waiting for AC power."));

    probe.ac = true;
    QVERIFY(BackupPrerequisites::check(set, probe).ready);

    probe.ac = false;
    set.onlyOnAcPower = false;
    QVERIFY(BackupPrerequisites::check(set, probe).ready);
}

QTEST_MAIN(ReliabilityTest)
#include "reliability_test.moc"

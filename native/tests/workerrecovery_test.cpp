#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

#include "../src/backupconfig.h"
#include "../src/backuprunstore.h"

class WorkerRecoveryTest : public QObject
{
    Q_OBJECT

private slots:
    void recoveryPreservesTheLogicalRun();
    void terminatedWorkerRecovers_data();
    void terminatedWorkerRecovers();
};

void WorkerRecoveryTest::recoveryPreservesTheLogicalRun()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    BackupRunStore store(home.filePath("runs.json"));
    const QDateTime scheduled(QDate(2026, 1, 1), QTime(12, 0));
    const QDateTime now = scheduled.addDays(1);
    QVERIFY(store.enqueue("documents", "schedule", scheduled));
    auto &record = *store.find("documents");
    store.markRunning(record);
    record.remoteCopyPath = "/interrupted-copy";
    record.lastSuccess = scheduled.addDays(-1);
    record.lastScheduled = scheduled;
    record.nextScheduled = now;
    record.progress = {100, 100, 1, 1, true};
    record.progressElapsedMs = 1000;
    record.progressUpdatedAt = scheduled;
    QVERIFY(store.enqueue("pending", "manual", now));
    store.ensureSet("idle");
    QVERIFY(store.save());

    BackupRunStore reopened(store.filePath());
    QVERIFY(reopened.load());
    QCOMPARE(reopened.readyIndexes(now).size(), 1);
    QVERIFY(reopened.recoverInterrupted(now));
    QVERIFY(reopened.save());
    QVERIFY(reopened.load());
    const auto &recovered = *reopened.find("documents");
    QCOMPARE(reopened.records().size(), 3);
    QCOMPARE(recovered.status, QString("retrying"));
    QCOMPARE(recovered.reason, QString("schedule"));
    QCOMPARE(recovered.scheduledFor, scheduled);
    QCOMPARE(recovered.lastScheduled, scheduled);
    QCOMPARE(recovered.nextScheduled, now);
    QCOMPARE(recovered.lastSuccess, scheduled.addDays(-1));
    QCOMPARE(recovered.lastFailure, now);
    QCOMPARE(recovered.nextAttempt, now);
    QCOMPARE(recovered.attempts, 1);
    QCOMPARE(recovered.remoteCopyPath, QString("/interrupted-copy"));
    QVERIFY(recovered.lastError.contains("interrupted"));
    QCOMPARE(recovered.progress.totalFiles, 0);
    QVERIFY(!recovered.progress.finalizing);
    QCOMPARE(recovered.progressElapsedMs, qint64(0));
    QVERIFY(!recovered.progressUpdatedAt.isValid());
    QCOMPARE(reopened.readyIndexes(now).size(), 2);
    QCOMPARE(reopened.find("pending")->status, QString("pending"));
    QCOMPARE(reopened.find("idle")->status, QString("idle"));
    QVERIFY(!reopened.enqueue("documents", "manual", now));
    QVERIFY(!reopened.recoverInterrupted(now));
}

void WorkerRecoveryTest::terminatedWorkerRecovers_data()
{
    QTest::addColumn<QString>("phase");
    QTest::addColumn<bool>("verificationFails");
    QTest::newRow("upload") << QString("upload") << false;
    QTest::newRow("manifest finalization") << QString("manifest") << false;
    QTest::newRow("upload retry verification fails") << QString("upload") << true;
    QTest::newRow("manifest retry verification fails") << QString("manifest") << true;
}

void WorkerRecoveryTest::terminatedWorkerRecovers()
{
    QFETCH(QString, phase);
    QFETCH(bool, verificationFails);
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
    mkdir -p "$FAKE_REMOTE$parent"
    cp "$source" "$FAKE_REMOTE$parent/$name"
    if [[ "$BLOCK_PHASE" == upload && "$name" != manifest.json ]] ||
       [[ "$BLOCK_PHASE" == manifest && "$name" == manifest.json ]]; then
      printf '%s' "$parent/$name" > "$BLOCK_MARKER"
      # Exit when the worker is killed, so its CLI child cannot leak or
      # continue changing the remote fixture during the restarted attempt.
      while kill -0 "$PPID" 2>/dev/null; do sleep 0.05; done
      exit 1
    fi
    ;;
  info)
    if [[ "$FAIL_VERIFICATION" == yes ]]; then
      printf 'Remote verification failed' >&2
      exit 1
    fi
    path="$FAKE_REMOTE${@: -1}"
    [[ -f "$path" ]] || exit 1
    checksum="$(sha256sum "$path")"
    printf '{"size":%s,"sha256":"%s"}' "$(stat -c %s "$path")" "${checksum%% *}"
    ;;
  *) exit 1 ;;
esac
)CLI");
    cli.close();
    QVERIFY(cli.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    QFile source(home.filePath("document.txt"));
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("recover this document");
    source.close();
    BackupConfig config;
    config.protonBinary = cli.fileName();
    config.sets = {{"documents", "Documents", "/my-files/backups", {source.fileName()}, {}}};
    const QString configPath = home.filePath("settings.json");
    QVERIFY(BackupConfigStore(configPath).save(config));
    const QString alternateConfigPath = home.filePath("alternate-settings.json");
    QVERIFY(BackupConfigStore(alternateConfigPath).save(config));
    BackupRunStore store(home.filePath("omacustos-backup-runs.json"));
    const QDateTime scheduled = QDateTime::currentDateTimeUtc();
    QVERIFY(store.enqueue("documents", "manual", scheduled));
    QVERIFY(store.save());
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("FAKE_REMOTE", home.filePath("remote"));
    environment.insert("BLOCK_MARKER", home.filePath("blocked"));
    environment.insert("BLOCK_PHASE", phase);
    environment.insert("FAIL_VERIFICATION", "no");
    QProcess worker;
    worker.setProcessEnvironment(environment);
    worker.start(QStringLiteral(OMACUSTOS_WORKER_BINARY), {"--config", configPath});
    QVERIFY(worker.waitForStarted());
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(home.filePath("blocked")), 10000);
    // A blocked synchronous transfer still publishes its latest phase without
    // needing another engine callback, while ordinary phase changes coalesce.
    const QString expectedPhase = phase == "upload" ? "uploading" : "finalizing";
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        return store.load() && store.find("documents")
            && store.find("documents")->progress.phase == expectedPhase;
    })(), 3000);
    QVERIFY(store.load());
    QCOMPARE(store.find("documents")->status, QString("running"));
    QCOMPARE(store.find("documents")->attempts, 1);
    QCOMPARE(store.find("documents")->progress.finalizing, phase == "manifest");
    const QString interruptedCopy = store.find("documents")->remoteCopyPath;
    QVERIFY(!interruptedCopy.isEmpty());
    QVERIFY(!store.find("documents")->lastSuccess.isValid());

    // Old lock timestamps must not allow a second worker to recover a live
    // run. The alternate config also exercises the shared queue-worker lock.
    for (const QString &lockPath : {configPath + ".worker.lock", store.filePath() + ".worker.lock"}) {
        QFile lock(lockPath);
        QVERIFY(lock.open(QIODevice::ReadWrite));
        QVERIFY(lock.setFileTime(scheduled.addSecs(-120), QFileDevice::FileModificationTime));
    }
    QFile state(store.filePath());
    QVERIFY(state.open(QIODevice::ReadOnly));
    const QByteArray runningState = state.readAll();
    state.close();
    for (const QString &path : {configPath, alternateConfigPath}) {
        QProcess competitor;
        competitor.setProcessEnvironment(environment);
        competitor.start(QStringLiteral(OMACUSTOS_WORKER_BINARY), {"--config", path});
        QVERIFY(competitor.waitForFinished(5000));
        QCOMPARE(competitor.exitCode(), 0);
        QVERIFY(state.open(QIODevice::ReadOnly));
        QCOMPARE(state.readAll(), runningState);
        state.close();
    }

    worker.kill();
    QVERIFY(worker.waitForFinished(5000));
    QCOMPARE(worker.exitStatus(), QProcess::CrashExit);
    environment.insert("BLOCK_PHASE", "none");
    environment.insert("FAIL_VERIFICATION", verificationFails ? "yes" : "no");
    QProcess restarted;
    restarted.setProcessEnvironment(environment);
    restarted.start(QStringLiteral(OMACUSTOS_WORKER_BINARY), {"--config", configPath});
    QVERIFY(restarted.waitForFinished(10000));
    QCOMPARE(restarted.exitStatus(), QProcess::NormalExit);
    QVERIFY2(restarted.exitCode() == 0, restarted.readAllStandardError().constData());
    QVERIFY(store.load());
    QCOMPARE(store.records().size(), 1);
    const auto &record = *store.find("documents");
    QCOMPARE(record.reason, QString("manual"));
    QCOMPARE(record.scheduledFor, scheduled);
    QVERIFY(record.remoteCopyPath != interruptedCopy);
    if (verificationFails) {
        QVERIFY(record.status != "success");
        QVERIFY(record.status != "running");
        QVERIFY(!record.lastSuccess.isValid());
        QVERIFY(record.lastFailure.isValid());
        QVERIFY(record.nextAttempt.isValid());
        QCOMPARE(record.attempts, 2);
    } else {
        QCOMPARE(record.status, QString("success"));
        QVERIFY(record.lastSuccess.isValid());
        QVERIFY(!record.nextAttempt.isValid());
        QCOMPARE(record.attempts, 0);
        QVERIFY(store.readyIndexes(QDateTime::currentDateTimeUtc()).isEmpty());
        QFile restored(home.filePath("remote") + record.remoteCopyPath + "/document.txt");
        QVERIFY(restored.open(QIODevice::ReadOnly));
        QCOMPARE(restored.readAll(), QByteArray("recover this document"));
        QVERIFY(QFile::exists(home.filePath("remote") + record.remoteCopyPath + "/manifest.json"));
    }
}

QTEST_GUILESS_MAIN(WorkerRecoveryTest)
#include "workerrecovery_test.moc"

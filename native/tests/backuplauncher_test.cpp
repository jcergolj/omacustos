#include "../src/backuplauncher.h"
#include "../src/backupconfig.h"
#include "../src/backuprunstore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

namespace {

class EnvironmentOverride
{
public:
    EnvironmentOverride(const char *name, const QByteArray &value)
        : name(name), wasSet(qEnvironmentVariableIsSet(name)), previous(qgetenv(name))
    {
        qputenv(name, value);
    }

    ~EnvironmentOverride()
    {
        if (wasSet) qputenv(name.constData(), previous);
        else qunsetenv(name.constData());
    }

private:
    QByteArray name;
    bool wasSet;
    QByteArray previous;
};

bool write(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

bool installProtonFixture(const QString &path)
{
    return write(path, R"CLI(#!/bin/bash
set -eu
if [[ -n "${CLI_CALLS:-}" ]]; then touch "$CLI_CALLS"; fi
case "$2" in
  list) printf '[]' ;;
  upload)
    source="${@: -2:1}"
    parent="${@: -1}"
    name="$(basename "$source")"
    if [[ "$name" == manifest.json && "$parent" == */A/* ]]; then
      if [[ "${OUTCOME:-success}" == failed ]]; then printf 'Connection interrupted' >&2; exit 1; fi
      if [[ "${OUTCOME:-success}" == authentication_required ]]; then printf 'Authentication required' >&2; exit 1; fi
    fi
    mkdir -p "$FAKE_REMOTE$parent"
    cp -f "$source" "$FAKE_REMOTE$parent/$name"
    if [[ -n "${BARRIERS:-}" && ( "$name" == a.txt || "$name" == b.txt ) ]]; then
      touch "$BARRIERS/$name.started"
      while [[ ! -f "$BARRIERS/$name.release" ]]; do
        kill -0 "$PPID" 2>/dev/null || exit 1
        sleep 0.05
      done
    fi
    ;;
  info)
    path="$FAKE_REMOTE${@: -1}"
    [[ -f "$path" ]] || exit 1
    checksum="$(sha256sum "$path")"
    printf '{"size":%s,"sha256":"%s"}' "$(stat -c %s "$path")" "${checksum%% *}"
    ;;
  *) exit 1 ;;
esac
)CLI") && QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

}

class BackupLauncherTest final : public QObject
{
    Q_OBJECT

private slots:
    void queuesDuringTransfer_data();
    void queuesDuringTransfer();
    void waitingTransitionsSurviveExit_data();
    void waitingTransitionsSurviveExit();
    void queuedWorkerWaitsForCopyManagement();
    void queuesConfiguredBackups_data()
    {
        QTest::addColumn<QString>("entryPoint");
        QTest::newRow("legacy source/destination entry point") << "legacy-launcher";
        QTest::newRow("legacy file consumer") << "legacy-file";
        QTest::newRow("modern file consumer") << "modern-file";
        QTest::newRow("selected modern set") << "selected-set";
    }

    void queuesConfiguredBackups()
    {
        QFETCH(QString, entryPoint);
        QTemporaryDir home;
        QVERIFY(home.isValid());
        EnvironmentOverride homeOverride("HOME", home.path().toUtf8());
        EnvironmentOverride pathOverride("PATH", home.path().toUtf8());
        EnvironmentOverride binaryOverride("OMACUSTOS_PROTON_BIN", "/custom/proton-drive");
        const QString systemctl = home.filePath("systemctl");
        const QByteArray script = "#!/bin/sh\n"
            "if [ \"$*\" != '--user --no-block start omacustos.service' ]; then exit 2; fi\n"
            "printf '%s\\n' \"$*\" > \"${0%/*}/calls\"\n";
        QVERIFY(write(systemctl, script));
        QVERIFY(QFile::setPermissions(systemctl, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const QString configPath = home.filePath(".config/omacustos/omacustos-backup.json");
        const QString runPath = home.filePath(".config/omacustos/omacustos-backup-runs.json");
        const QString source = home.filePath("Documents");
        const QString remote = "/my-files/backups";
        const bool legacy = entryPoint.startsWith("legacy");
        const QString id = legacy ? "default" : "documents";
        if (entryPoint == "legacy-file") {
            QVERIFY(QDir().mkpath(QFileInfo(configPath).absolutePath()));
            QVERIFY(write(configPath, QJsonDocument(QJsonObject {
                {"source_directory", source}, {"remote_root", remote}, {"proton_binary", "/custom/proton-drive"},
            }).toJson()));
        } else if (!legacy) {
            BackupConfig config;
            config.protonBinary = "/custom/proton-drive";
            config.sets = {
                {id, "Documents", remote, {source}, {}},
                {"photos", "Photos", remote, {home.filePath("Photos")}, {}},
            };
            QVERIFY(BackupConfigStore(configPath).save(config));
        }

        BackupLauncher launcher;
        QSignalSpy started(&launcher, &BackupLauncher::started);
        QSignalSpy failed(&launcher, &BackupLauncher::failed);
        if (entryPoint == "legacy-launcher") launcher.startBackup(source, remote);
        else if (entryPoint == "selected-set") launcher.startBackup(id);
        else launcher.startBackup();
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(started.count(), 1);

        BackupConfig saved;
        QVERIFY(BackupConfigStore(configPath).load(&saved));
        QCOMPARE(saved.protonBinary, QStringLiteral("/custom/proton-drive"));
        QCOMPARE(saved.sets.first().id, id);
        QCOMPARE(saved.sets.first().name, legacy ? QString("Default backup") : QString("Documents"));
        QCOMPARE(saved.sets.first().sourceDirectories, QStringList {source});
        QCOMPARE(saved.sets.first().remoteRoot, remote);
        if (entryPoint == "legacy-launcher") {
            QFile file(configPath);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();
            QCOMPARE(document.value("sets").toArray().size(), 1);
            QVERIFY(!document.contains("source_directory"));
            QVERIFY(!document.contains("remote_root"));
        }
        BackupRunStore runs(runPath);
        QVERIFY(runs.load());
        QVERIFY(runs.find(id) != nullptr);
        QCOMPARE(runs.find(id)->status, QStringLiteral("pending"));
        QCOMPARE(runs.find(id)->reason, QStringLiteral("manual"));
        if (entryPoint == "modern-file") {
            QVERIFY(runs.find("photos") != nullptr);
            QCOMPARE(runs.find("photos")->status, QStringLiteral("pending"));
        } else {
            QVERIFY(runs.find("photos") == nullptr);
        }
        QFile calls(home.filePath("calls"));
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(calls.readAll(), QByteArray("--user --no-block start omacustos.service\n"));
    }
};

void BackupLauncherTest::queuesDuringTransfer_data()
{
    QTest::addColumn<bool>("restart");
    QTest::addColumn<QString>("outcome");
    QTest::newRow("progress and success preserve enqueue") << false << QString("success");
    QTest::newRow("progress and failure preserve enqueue") << false << QString("failed");
    QTest::newRow("progress and incomplete preserve enqueue") << false << QString("incomplete");
    QTest::newRow("authentication failure preserves enqueue") << false << QString("authentication_required");
    QTest::newRow("interrupted worker preserves enqueue") << true << QString("success");
}

void BackupLauncherTest::queuesDuringTransfer()
{
    QFETCH(bool, restart);
    QFETCH(QString, outcome);
    QTemporaryDir home;
    QVERIFY(home.isValid());
    EnvironmentOverride homeOverride("HOME", home.path().toUtf8());
    EnvironmentOverride pathOverride("PATH", home.path().toUtf8() + ':' + qgetenv("PATH"));
    QVERIFY(write(home.filePath("systemctl"), "#!/bin/sh\nexit 0\n"));
    QVERIFY(QFile::setPermissions(home.filePath("systemctl"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const QString cli = home.filePath("fake-proton");
    QVERIFY(installProtonFixture(cli));
    QVERIFY(write(home.filePath("a.txt"), "first backup"));
    QVERIFY(write(home.filePath("b.txt"), "second backup"));
    BackupConfig config;
    config.protonBinary = cli;
    config.sets = {{"a", "A", "/my-files/backups", {home.filePath("a.txt")}, {}},
                   {"b", "B", "/my-files/backups", {home.filePath("b.txt")}, {}}};
    if (outcome == "incomplete") config.sets[0].sourceDirectories.append(home.filePath("missing.txt"));
    const QString configPath = home.filePath(".config/omacustos/omacustos-backup.json");
    QVERIFY(BackupConfigStore(configPath).save(config));
    BackupLauncher launcher;
    QSignalSpy failed(&launcher, &BackupLauncher::failed);
    QSignalSpy started(&launcher, &BackupLauncher::started);
    launcher.startBackup("a");
    QCOMPARE(started.count(), 1);
    BackupRunStore store(home.filePath(".config/omacustos/omacustos-backup-runs.json"));
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("FAKE_REMOTE", home.filePath("remote"));
    environment.insert("BARRIERS", home.path());
    environment.insert("OUTCOME", outcome);
    QProcess worker;
    worker.setProcessEnvironment(environment);
    worker.start(QStringLiteral(OMACUSTOS_WORKER_BINARY), {"--config", configPath});
    QVERIFY(worker.waitForStarted());
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(home.filePath("a.txt.started")), 10000);
    launcher.startBackup("b");
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QCOMPARE(started.count(), 2);
    QVERIFY(store.load());
    const QDateTime scheduled = store.find("b")->scheduledFor;
    QCOMPARE(store.find("b")->status, QString("pending"));
    launcher.startBackup("a");
    launcher.startBackup("b");
    QCOMPARE(failed.count(), 2); // Running/pending requests coalesce.
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        return store.load() && store.find("a")->progress.phase == "uploading";
    })(), 3000);
    QCOMPARE(store.find("b")->status, QString("pending"));
    QCOMPARE(store.find("b")->scheduledFor, scheduled);
    const QString firstCopy = store.find("a")->remoteCopyPath;
    if (restart) {
        worker.kill();
        QVERIFY(worker.waitForFinished(5000));
        QVERIFY(store.load());
        QCOMPARE(store.find("b")->status, QString("pending"));
    }
    QVERIFY(write(home.filePath("a.txt.release"), ""));
    if (restart) {
        worker.start(QStringLiteral(OMACUSTOS_WORKER_BINARY), {"--config", configPath});
        QVERIFY(worker.waitForStarted());
    }
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(home.filePath("b.txt.started")), 10000);
    QVERIFY(store.load());
    const QString expectedOutcome = outcome == "failed" ? QString("retrying") : outcome;
    QCOMPARE(store.find("a")->status, expectedOutcome);
    QCOMPARE(store.find("a")->remoteCopyPath, firstCopy);
    QCOMPARE(store.find("b")->status, QString("running"));
    QCOMPARE(store.find("b")->scheduledFor, scheduled);
    QCOMPARE(store.find("b")->reason, QString("manual"));
    QVERIFY(write(home.filePath("b.txt.release"), ""));
    QVERIFY(worker.waitForFinished(10000));
    QVERIFY2(worker.exitCode() == 0, worker.readAllStandardError().constData());
    QVERIFY(store.load());
    QCOMPARE(store.find("a")->status, expectedOutcome);
    QCOMPARE(store.find("b")->status, QString("success"));
    QCOMPARE(store.find("b")->scheduledFor, scheduled);
    QCOMPARE(store.records().size(), 2);
}

void BackupLauncherTest::waitingTransitionsSurviveExit_data()
{
    QTest::addColumn<QString>("ordering");
    QTest::newRow("every ready set waits") << QString("all-waiting");
    QTest::newRow("waiting before active") << QString("waiting-first");
    QTest::newRow("waiting after active") << QString("waiting-last");
}

void BackupLauncherTest::waitingTransitionsSurviveExit()
{
    QFETCH(QString, ordering);
    QTemporaryDir home;
    QVERIFY(home.isValid());
    EnvironmentOverride homeOverride("HOME", home.path().toUtf8());
    EnvironmentOverride pathOverride("PATH", home.path().toUtf8() + ':' + qgetenv("PATH"));
    QVERIFY(write(home.filePath("systemctl"), "#!/bin/sh\nexit 0\n"));
    QVERIFY(QFile::setPermissions(home.filePath("systemctl"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const QString cli = home.filePath("fake-proton");
    QVERIFY(installProtonFixture(cli));
    QVERIFY(write(home.filePath("document.txt"), "document"));
    BackupSet waiting {"waiting", "Waiting", "/my-files/backups", {home.filePath("document.txt")}, {}};
    waiting.onlyOnAcPower = true;
    // A due schedule must coalesce with the existing manual logical run.
    waiting.schedule = {"daily", 0, 0, 1, 1};
    BackupSet other {"other", "Other", "/my-files/backups", {home.filePath("document.txt")}, {}};
    other.onlyOnAcPower = ordering == "all-waiting";
    BackupConfig config;
    config.protonBinary = cli;
    config.sets = ordering == "waiting-last" ? QVector<BackupSet>{other, waiting} : QVector<BackupSet>{waiting, other};
    const QString configPath = home.filePath(".config/omacustos/omacustos-backup.json");
    QVERIFY(BackupConfigStore(configPath).save(config));
    BackupLauncher launcher;
    QSignalSpy failed(&launcher, &BackupLauncher::failed);
    launcher.startBackup();
    QVERIFY(failed.isEmpty());
    BackupRunStore store(home.filePath(".config/omacustos/omacustos-backup-runs.json"));
    QVERIFY(store.load());
    const QDateTime scheduled = store.find("waiting")->scheduledFor;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("FAKE_REMOTE", home.filePath("remote"));
    environment.insert("CLI_CALLS", home.filePath("cli-calls"));
    QProcess worker;
    worker.setProcessEnvironment(environment);
    worker.start(QStringLiteral(OMACUSTOS_WAITING_WORKER_BINARY), {"--config", configPath});
    QVERIFY(worker.waitForFinished(10000));
    QVERIFY2(worker.exitCode() == 0, worker.readAllStandardError().constData());
    QVERIFY(store.load());
    const auto waitingRun = *store.find("waiting");
    QCOMPARE(waitingRun.status, QString("waiting"));
    QCOMPARE(waitingRun.lastError, QString("Waiting for AC power."));
    QCOMPARE(waitingRun.reason, QString("manual"));
    QCOMPARE(waitingRun.scheduledFor, scheduled);
    QCOMPARE(waitingRun.attempts, 0);
    QVERIFY(waitingRun.nextAttempt >= scheduled.addSecs(60));
    QVERIFY(waitingRun.nextAttempt <= QDateTime::currentDateTime().addSecs(60));
    QVERIFY(waitingRun.lastScheduled.isValid());
    QVERIFY(waitingRun.lastScheduled <= scheduled);
    QVERIFY(waitingRun.nextScheduled > scheduled);
    QCOMPARE(store.find("other")->status, ordering == "all-waiting" ? QString("waiting") : QString("success"));
    if (ordering == "all-waiting") {
        QCOMPARE(store.find("other")->lastError, QString("Waiting for AC power."));
        QVERIFY(store.find("other")->nextAttempt.isValid());
        QVERIFY(!QFile::exists(home.filePath("cli-calls")));
    }
    // A fresh worker reads the deferred attempt and does not reset its identity,
    // backoff, or schedule catch-up on an immediate restart.
    worker.start(QStringLiteral(OMACUSTOS_WAITING_WORKER_BINARY), {"--config", configPath});
    QVERIFY(worker.waitForFinished(10000));
    QCOMPARE(worker.exitCode(), 0);
    QVERIFY(store.load());
    QCOMPARE(store.find("waiting")->nextAttempt, waitingRun.nextAttempt);
    QCOMPARE(store.find("waiting")->scheduledFor, scheduled);
    QCOMPARE(store.find("waiting")->lastScheduled, waitingRun.lastScheduled);
    QCOMPARE(store.find("waiting")->nextScheduled, waitingRun.nextScheduled);
}

void BackupLauncherTest::queuedWorkerWaitsForCopyManagement()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    EnvironmentOverride homeOverride("HOME", home.path().toUtf8());
    EnvironmentOverride pathOverride("PATH", home.path().toUtf8() + ':' + qgetenv("PATH"));
    QVERIFY(write(home.filePath("systemctl"), "#!/bin/sh\nexit 0\n"));
    QVERIFY(QFile::setPermissions(home.filePath("systemctl"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const QString cli = home.filePath("fake-proton");
    QVERIFY(installProtonFixture(cli));
    QVERIFY(write(home.filePath("document.txt"), "manual-only backup"));
    BackupConfig config;
    config.protonBinary = cli;
    config.sets = {{"documents", "Documents", "/my-files/backups", {home.filePath("document.txt")}, {}}};
    const QString configPath = home.filePath(".config/omacustos/omacustos-backup.json");
    const QString runPath = home.filePath(".config/omacustos/omacustos-backup-runs.json");
    QVERIFY(BackupConfigStore(configPath).save(config));
    // Model a remote-management call holding exclusion while the launcher
    // durably queues work and invokes the service's real worker executable.
    QLockFile managementGate(runPath + ".management.lock");
    QLockFile configLock(configPath + ".worker.lock");
    QLockFile workerLock(runPath + ".worker.lock");
    QVERIFY(managementGate.tryLock(0));
    QVERIFY(configLock.tryLock(0));
    QVERIFY(workerLock.tryLock(0));
    BackupLauncher launcher;
    QSignalSpy failed(&launcher, &BackupLauncher::failed);
    launcher.startBackup("documents");
    QVERIFY(failed.isEmpty());
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("FAKE_REMOTE", home.filePath("remote"));
    environment.insert("CLI_CALLS", home.filePath("cli-calls"));
    QProcess worker;
    worker.setProcessEnvironment(environment);
    worker.start(QStringLiteral(OMACUSTOS_WORKER_BINARY), {"--config", configPath});
    QVERIFY(worker.waitForStarted());
    QVERIFY(!worker.waitForFinished(300));
    QVERIFY(!QFile::exists(home.filePath("cli-calls")));
    workerLock.unlock();
    configLock.unlock();
    managementGate.unlock();
    QVERIFY(worker.waitForFinished(10000));
    QVERIFY2(worker.exitCode() == 0, worker.readAllStandardError().constData());
    BackupRunStore runs(runPath);
    QVERIFY(runs.load());
    QCOMPARE(runs.find("documents")->status, QString("success"));
    QCOMPARE(runs.find("documents")->reason, QString("manual"));
}

QTEST_GUILESS_MAIN(BackupLauncherTest)
#include "backuplauncher_test.moc"

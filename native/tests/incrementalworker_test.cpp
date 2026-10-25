#include "incrementalworker.h"
#include "incrementalrestore.h"
#include "localprovider.h"
#include "backuprunstore.h"
#include "backupmanifest.h"
#include "backuprestorecontroller.h"
#include "incrementalmaintenance.h"
#include "backupsetcontroller.h"
#include "protonprovider.h"
#include "qprocessrunner.h"
#include <QSignalSpy>
#include <QScopeGuard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <sys/stat.h>

namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool write(const QString &path, const QByteArray &bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray bytes(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
struct Fixture {
    QTemporaryDir directory{QDir::current().filePath("incremental-worker-XXXXXX")};
    BackupSet set{"documents", "Documents", "backups", {directory.filePath("source")}, {}};
    LocalProvider provider{directory.filePath("remote")};
    QString state = directory.filePath("state");
    QString error;
    Fixture() { set.stagingDirectory = directory.filePath("stage"); set.stagingBudget = 8 * 1024 * 1024; }
    bool prepare() { return directory.isValid() && QDir().mkpath(directory.filePath("remote")) && write(directory.filePath("source/a.txt"), "first") && write(directory.filePath("source/b.txt"), "unchanged"); }
    bool request(IncrementalWriteRequest *request) { return IncrementalWorker::request(set, state, uuid(), provider, request, &error); }
};

struct ProcessFixture : Fixture {
    QString configPath = directory.filePath("settings.json");
    BackupRunStore runs{directory.filePath("omacustos-backup-runs.json")};
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    bool prepareProcess()
    {
        if (!prepare()) return false;
        set.remoteRoot = "/my-files/backups";
        const QString cli = directory.filePath("proton-fixture");
        if (!QFile::copy(QStringLiteral(OMACUSTOS_CONTINUATION_CLI), cli)
            || !QFile::setPermissions(cli, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) return false;
        BackupConfig config; config.protonBinary = cli; config.sets = {set};
        if (!BackupConfigStore(configPath).save(config, &error)) return false;
        environment.remove("OMACUSTOS_INTERNAL_LEGACY_WORKER");
        environment.insert("FIXTURE_REMOTE", directory.filePath("remote"));
        environment.insert("FIXTURE_LOG", directory.filePath("transfers.jsonl"));
        environment.insert("FIXTURE_BATCH_LOG", directory.filePath("batches.jsonl"));
        environment.insert("FIXTURE_MARKER", directory.filePath("blocked"));
        environment.insert("FIXTURE_EXACT_IDENTITY", "yes");
        return true;
    }
    void start(QProcess &worker)
    {
        worker.setProcessEnvironment(environment);
        worker.start(QStringLiteral(OMACUSTOS_WORKER_BINARY), {"--config", configPath});
    }
    bool run(bool enqueue = true)
    {
        if (enqueue && !runs.queueManual({set.id}, true, &error)) return false;
        QProcess worker; start(worker);
        if (!worker.waitForFinished(60000) || worker.exitCode() != 0) { error = worker.readAllStandardError(); return false; }
        if (!runs.load(&error)) return false;
        if (runs.find(set.id)->status != "success") { error = runs.find(set.id)->lastError + worker.readAllStandardError(); return false; }
        return true;
    }
    int archiveUploads() const
    {
        int count = 0;
        for (const auto &line : bytes(directory.filePath("transfers.jsonl")).split('\n'))
            if (QJsonDocument::fromJson(line).object().value("remote").toString().endsWith(".tar.gz")) ++count;
        return count;
    }
};

class FixtureEnvironment
{
public:
    explicit FixtureEnvironment(const ProcessFixture &fixture)
    {
        for (const auto &name : {QByteArray("FIXTURE_REMOTE"), QByteArray("FIXTURE_LOG"), QByteArray("FIXTURE_BATCH_LOG"),
                 QByteArray("FIXTURE_MARKER"), QByteArray("FIXTURE_EXACT_IDENTITY"),
                 QByteArray("FIXTURE_BLOCK_ITEM"), QByteArray("FIXTURE_RELEASE")}) {
            previous.insert(name, qgetenv(name.constData()));
            if (fixture.environment.contains(QString::fromUtf8(name)))
                qputenv(name.constData(), fixture.environment.value(QString::fromUtf8(name)).toUtf8());
            else qunsetenv(name.constData());
        }
    }
    ~FixtureEnvironment()
    {
        for (auto it = previous.cbegin(); it != previous.cend(); ++it) {
            if (it.value().isNull()) qunsetenv(it.key().constData());
            else qputenv(it.key().constData(), it.value());
        }
    }
private:
    QMap<QByteArray, QByteArray> previous;
};
}

class IncrementalWorkerTest : public QObject
{
    Q_OBJECT
private slots:
    void baselineUnchangedEditAndRestore()
    {
        Fixture f; QVERIFY(f.prepare());
        IncrementalWriteRequest first; QVERIFY2(f.request(&first), qPrintable(f.error));
        QString warning;
        const auto saved = IncrementalWorker::run(f.set, first, f.provider, {}, [&](const auto &message) { warning = message; });
        QVERIFY2(saved.success(), saved.issues.isEmpty() ? "save failed" : qPrintable(saved.issues.first().reason));
        QVERIFY(warning.contains("PLUS"));
        QVERIFY(warning.contains("not a quota guarantee"));
        QCOMPARE(saved.savedFiles, 2);
        IncrementalWriteRequest next; QVERIFY2(f.request(&next), qPrintable(f.error));
        const auto unchanged = IncrementalWorker::run(f.set, next, f.provider);
        QCOMPARE(unchanged.state, IncrementalWriteResult::State::Unchanged);
        QCOMPARE(unchanged.lastSavedChange, saved.lastSavedChange);
        QCOMPARE(unchanged.latestSnapshotPath, saved.latestSnapshotPath);
        QVERIFY(unchanged.manifestPath.isEmpty());
        QVERIFY(write(f.directory.filePath("source/a.txt"), "changed"));
        QVERIFY(QFile::rename(f.directory.filePath("source/b.txt"), f.directory.filePath("source/renamed.txt")));
        IncrementalWriteRequest edited; QVERIFY2(f.request(&edited), qPrintable(f.error));
        const auto result = IncrementalWorker::run(f.set, edited, f.provider);
        QCOMPARE(result.state, IncrementalWriteResult::State::Saved);
        for (const auto &snapshot : {saved.latestSnapshotPath, result.latestSnapshotPath}) {
            RemoteCopy copy;
            QVERIFY2(IncrementalRestore::verifyCopy(f.provider, snapshot, first.scan.setId, &copy, &f.error), qPrintable(f.error));
            BackupEngine engine;
            const QString destination = f.directory.filePath("restored/" + copy.copyId);
            const auto restored = engine.restoreFiles({copy.entries, destination, snapshot}, f.provider);
            QVERIFY2(restored.success, qPrintable(restored.error));
            QCOMPARE(bytes(destination + "/a.txt"), snapshot == saved.latestSnapshotPath ? QByteArray("first") : QByteArray("changed"));
            QCOMPARE(bytes(destination + (snapshot == saved.latestSnapshotPath ? "/b.txt" : "/renamed.txt")), QByteArray("unchanged"));
        }
    }

    void incompleteScanDoesNotPublish()
    {
        Fixture f; QVERIFY(f.prepare());
        IncrementalWriteRequest first; QVERIFY2(f.request(&first), qPrintable(f.error));
        QVERIFY(IncrementalWorker::run(f.set, first, f.provider).success());
        f.set.sourceDirectories.append(f.directory.filePath("missing"));
        IncrementalWriteRequest next; QVERIFY2(f.request(&next), qPrintable(f.error));
        const auto failed = IncrementalWorker::run(f.set, next, f.provider);
        QVERIFY(!failed.success());
        QVERIFY(!failed.manifestCommitted);
        QVERIFY(!QFile::exists(f.directory.filePath("remote/") + next.remoteSetRoot + "/snapshots/" + next.snapshotId + "/manifest.json"));
    }

    void skippedLinksAndSpecialNodesDoNotInvalidateRegularFiles()
    {
        Fixture f; QVERIFY(f.prepare());
        IncrementalWriteRequest first; QVERIFY2(f.request(&first), qPrintable(f.error));
        QVERIFY(IncrementalWorker::run(f.set, first, f.provider).success());
        QVERIFY(QFile::link(f.directory.filePath("source/a.txt"), f.directory.filePath("source/link")));
        QCOMPARE(::mkfifo(QFile::encodeName(f.directory.filePath("source/pipe")).constData(), 0600), 0);
        IncrementalWriteRequest next; QVERIFY2(f.request(&next), qPrintable(f.error));
        const auto result = IncrementalWorker::run(f.set, next, f.provider);
        QCOMPARE(result.state, IncrementalWriteResult::State::Unchanged);
    }

    void copiedImportedAndLostStateRequireTakeover()
    {
        Fixture f; QVERIFY(f.prepare());
        IncrementalWriteRequest first; QVERIFY2(f.request(&first), qPrintable(f.error));
        QVERIFY(IncrementalWorker::run(f.set, first, f.provider).success());
        auto imported = f.set; imported.readOnly = true;
        IncrementalWriteRequest denied;
        QVERIFY(!IncrementalWorker::request(imported, f.directory.filePath("imported"), uuid(), f.provider, &denied, &f.error));
        QVERIFY(f.error.contains("read-only"));
        const QString copied = f.directory.filePath("copied");
        QVERIFY(QDir().mkpath(copied + "/incremental-writers"));
        const QDir writers(f.state + "/incremental-writers");
        const auto states = writers.entryList({"*.json"}, QDir::Files);
        QCOMPARE(states.size(), 1);
        QVERIFY(QFile::copy(writers.filePath(states.first()), copied + "/incremental-writers/" + states.first()));
        QVERIFY(!IncrementalWorker::request(f.set, copied, uuid(), f.provider, &denied, &f.error));
        QVERIFY(f.error.contains("foreign"));
        const QString recovery = f.directory.filePath("recovery");
        QVERIFY(!IncrementalWorker::request(f.set, recovery, uuid(), f.provider, &denied, &f.error));
        QVERIFY(!IncrementalWorker::takeover(imported, recovery, f.provider, false, &f.error));
        QVERIFY2(IncrementalWorker::takeover(imported, recovery, f.provider, true, &f.error), qPrintable(f.error));
        QVERIFY2(IncrementalWorker::request(f.set, recovery, uuid(), f.provider, &denied, &f.error), qPrintable(f.error));
        QCOMPARE(denied.ownershipEpoch, 2);
        QVERIFY(denied.computerId != first.computerId);
        const auto stale = IncrementalWorker::run(f.set, first, f.provider);
        QVERIFY(!stale.success());
    }

    void copiedAndUnboundConfigurationIsReadOnly()
    {
        Fixture f; QVERIFY(f.prepare());
        BackupConfig config; config.sets = {f.set};
        const auto original = f.directory.filePath("original/settings.json");
        QVERIFY(BackupConfigStore(original).save(config, &f.error));
        BackupConfig loaded;
        QVERIFY(BackupConfigStore(original).load(&loaded, &f.error));
        QVERIFY(!loaded.sets.first().readOnly);
        const auto copied = f.directory.filePath("copied/settings.json");
        QVERIFY(QDir().mkpath(QFileInfo(copied).absolutePath()));
        QVERIFY(QFile::copy(original, copied));
        QVERIFY(BackupConfigStore(copied).load(&loaded, &f.error));
        QVERIFY(loaded.sets.first().readOnly);
        auto unbound = QJsonDocument::fromJson(bytes(original)).object();
        unbound.remove("installation_binding");
        const auto legacy = f.directory.filePath("unbound/settings.json");
        QVERIFY(write(legacy, QJsonDocument(unbound).toJson()));
        QVERIFY(BackupConfigStore(legacy).load(&loaded, &f.error));
        QVERIFY(loaded.sets.first().readOnly);
        const auto exported = f.directory.filePath("export.json");
        QVERIFY(BackupConfigStore(exported).exportSets(config, &f.error));
        QVERIFY(BackupConfigStore(exported).importSets(&loaded, &f.error));
        QVERIFY(loaded.sets.first().readOnly);
    }

    void explicitEnrollmentOfVerifiedEmptySet()
    {
        Fixture f; QVERIFY(f.prepare()); f.set.readOnly = true;
        IncrementalWriteRequest request;
        QVERIFY(!f.request(&request));
        QVERIFY(!QFile::exists(f.directory.filePath("remote/") + IncrementalWorker::root(f.set)));
        QVERIFY(!IncrementalWorker::takeover(f.set, f.state, f.provider, false, &f.error));
        QVERIFY2(IncrementalWorker::takeover(f.set, f.state, f.provider, true, &f.error), qPrintable(f.error));
        f.set.readOnly = false;
        QVERIFY2(f.request(&request), qPrintable(f.error));
        QCOMPARE(request.ownershipEpoch, 1);
        QVERIFY(IncrementalWorker::run(f.set, request, f.provider).success());
    }

    void configurationSourcesDoNotCaptureTheirOwnCoordinationState()
    {
        Fixture f; QVERIFY(f.prepare());
        const auto configPath = f.state + "/settings.json";
        const auto queue = f.state + "/omacustos-backup-runs.json";
        QVERIFY(write(configPath, "settings to preserve"));
        QVERIFY(write(configPath + ".lock", "volatile configuration lock"));
        QVERIFY(write(queue, "volatile queue"));
        f.set.sourceDirectories = {f.state};
        IncrementalWriteRequest first; QVERIFY2(f.request(&first), qPrintable(f.error));
        QVERIFY(!first.stateDirectory.startsWith(f.state + '/'));
        const auto saved = IncrementalWorker::run(f.set, first, f.provider, {}, {}, configPath);
        QVERIFY2(saved.success(), saved.issues.isEmpty() ? "save failed" : qPrintable(saved.issues.first().reason));
        QCOMPARE(saved.savedFiles, 1);
        IncrementalWriteRequest next; QVERIFY2(f.request(&next), qPrintable(f.error));
        int changes = 0;
        const auto result = IncrementalWorker::run(f.set, next, f.provider, [&](const QString &) {
            write(queue, QByteArray::number(++changes));
            return IncrementalControl::Continue;
        }, {}, configPath);
        QCOMPARE(result.state, IncrementalWriteResult::State::Unchanged);
        BackupEngine engine;
        auto preview = engine.preview(f.set.sourceDirectories, {});
        IncrementalWorker::excludeCoordinationFiles(&preview, configPath);
        QCOMPARE(preview.includedFiles, QStringList{configPath});
        QVERIFY(preview.excludedFiles.contains(queue));
        QVERIFY(preview.excludedFiles.contains(configPath + ".lock"));
    }

    void deferredCleanupDoesNotBlockScheduledWrites()
    {
        Fixture f; QVERIFY(f.prepare()); f.set.retention = 1;
        IncrementalWriteRequest first; QVERIFY2(f.request(&first), qPrintable(f.error));
        const auto original = IncrementalWorker::run(f.set, first, f.provider);
        QVERIFY(original.success());
        IncrementalWriteRequest latest;
        for (int index = 0; index < 2; ++index) {
            QVERIFY(write(f.directory.filePath("source/a.txt"), QByteArray::number(index)));
            IncrementalWriteRequest next; QVERIFY2(f.request(&next), qPrintable(f.error));
            const auto saved = IncrementalWorker::run(f.set, next, f.provider);
            latest = next;
            QVERIFY2(saved.success(), saved.issues.isEmpty() ? "save failed" : qPrintable(saved.issues.first().reason));
            QVERIFY(saved.cleanupNeedsAttention);
        }
        QVERIFY(QFile::exists(f.directory.filePath("remote/") + original.latestSnapshotPath + "/manifest.json"));
        QJsonObject proposal;
        QString confirmation;
        QVERIFY(!IncrementalRepository::cleanup(latest, f.provider, &proposal, &confirmation, &f.error));
        QVERIFY(!confirmation.isEmpty());
        QVERIFY(write(f.directory.filePath("source/a.txt"), "after abandoned preview"));
        IncrementalWriteRequest restarted; QVERIFY2(f.request(&restarted), qPrintable(f.error));
        const auto recovered = IncrementalWorker::run(f.set, restarted, f.provider);
        QVERIFY2(recovered.success(), recovered.issues.isEmpty() ? "recovery failed" : qPrintable(recovered.issues.first().reason));
        QVERIFY(QFile::exists(f.directory.filePath("remote/") + original.latestSnapshotPath + "/manifest.json"));
    }

    void authorizedCleanupRecoversWithoutExpandingScope()
    {
        Fixture f; QVERIFY(f.prepare()); f.set.retention = 1;
        IncrementalWriteRequest first; QVERIFY2(f.request(&first), qPrintable(f.error));
        const auto old = IncrementalWorker::run(f.set, first, f.provider);
        QVERIFY(old.success());
        QVERIFY(write(f.directory.filePath("source/a.txt"), "new"));
        IncrementalWriteRequest latest; QVERIFY2(f.request(&latest), qPrintable(f.error));
        const auto saved = IncrementalWorker::run(f.set, latest, f.provider);
        QVERIFY(saved.success());
        QJsonObject proposal;
        QString confirmation;
        QVERIFY(!IncrementalRepository::cleanup(latest, f.provider, &proposal, &confirmation, &f.error));
        QVERIFY(!confirmation.isEmpty());
        latest.cleanupConfirmation = confirmation;
        latest.checkpoint = [](const QString &phase) { return phase == "cleanup-authorized" ? IncrementalControl::Pause : IncrementalControl::Continue; };
        QVERIFY(!IncrementalRepository::cleanup(latest, f.provider, &proposal, &confirmation, &f.error));
        latest.checkpoint = {};
        QVERIFY2(IncrementalRepository::recoverCleanup(latest, f.provider, &f.error), qPrintable(f.error));
        QVERIFY(!QFile::exists(f.directory.filePath("remote/") + old.latestSnapshotPath + "/manifest.json"));
        RemoteCopy retained;
        QVERIFY2(IncrementalRestore::verifyCopy(f.provider, saved.latestSnapshotPath, latest.scan.setId, &retained, &f.error), qPrintable(f.error));
        QCOMPARE(retained.entries.size(), 2);
    }

    void normalWorkerUsesIncrementalByDefault_data()
    {
        QTest::addColumn<bool>("scheduled");
        QTest::newRow("manual") << false;
        QTest::newRow("scheduled") << true;
    }

    void normalWorkerUsesIncrementalByDefault()
    {
        QFETCH(bool, scheduled);
        ProcessFixture f;
        QVERIFY(write(f.directory.filePath("source/c.txt"), "deleted"));
        QVERIFY(f.prepareProcess());
        if (scheduled) {
            QVERIFY(f.runs.enqueue(f.set.id, "scheduled", QDateTime::currentDateTimeUtc()));
            QVERIFY(f.runs.save(&f.error));
        }
        QVERIFY2(f.run(!scheduled), qPrintable(f.error));
        const auto first = *f.runs.find(f.set.id);
        QCOMPARE(first.incremental.value("outcome").toString(), QString("saved"));
        QVERIFY(first.remoteCopyPath.contains("/sets/"));
        QVERIFY(!first.incremental.value("capacity_warning").toString().isEmpty());
        const auto transfers = bytes(f.directory.filePath("transfers.jsonl"));
        QVERIFY2(f.run(), qPrintable(f.error));
        const auto second = *f.runs.find(f.set.id);
        QCOMPARE(second.incremental.value("outcome").toString(), QString("unchanged"));
        QCOMPARE(second.remoteCopyPath, first.remoteCopyPath);
        QCOMPARE(second.incremental.value("last_saved_change"), first.incremental.value("last_saved_change"));
        const auto newTransfers = bytes(f.directory.filePath("transfers.jsonl")).mid(transfers.size());
        QVERIFY(!newTransfers.contains(".tar.gz"));
        QVERIFY(write(f.directory.filePath("source/a.txt"), "edited"));
        QVERIFY(QFile::rename(f.directory.filePath("source/b.txt"), f.directory.filePath("source/renamed.txt")));
        QVERIFY(QFile::remove(f.directory.filePath("source/c.txt")));
        QVERIFY2(f.run(), qPrintable(f.error));
        const auto current = f.runs.find(f.set.id)->remoteCopyPath;
        QVERIFY(current != first.remoteCopyPath);
        FixtureEnvironment environment(f);
        QProcessRunner runner(f.directory.filePath("proton-fixture"));
        ProtonProvider provider(runner);
        BackupEngine engine;
        for (const auto &snapshot : {first.remoteCopyPath, current}) {
            const bool old = snapshot == first.remoteCopyPath;
            RemoteCopy copy;
            QVERIFY(IncrementalRestore::verifyCopy(provider, snapshot, IncrementalWorker::setId(f.set), &copy, &f.error));
            const auto destination = f.directory.filePath("restored/" + QFileInfo(snapshot).fileName());
            const auto result = engine.restoreFiles({copy.entries, destination, snapshot}, provider);
            QVERIFY2(result.success, qPrintable(result.error));
            QCOMPARE(result.restoredCount, old ? 3 : 2);
            QCOMPARE(bytes(destination + "/a.txt"), old ? QByteArray("first") : QByteArray("edited"));
            QCOMPARE(bytes(destination + (old ? "/b.txt" : "/renamed.txt")), QByteArray("unchanged"));
            if (old) QCOMPARE(bytes(destination + "/c.txt"), QByteArray("deleted"));
            else QVERIFY(!QFile::exists(destination + "/c.txt"));
        }
    }

    void workerPauseCancelAndResume_data()
    {
        QTest::addColumn<QString>("action");
        QTest::newRow("pause") << QString("pause");
        QTest::newRow("cancel") << QString("cancel");
    }

    void workerPauseCancelAndResume()
    {
        QFETCH(QString, action);
        ProcessFixture f; QVERIFY(f.prepareProcess());
        f.environment.insert("FIXTURE_BLOCK_ARCHIVE_PHASE", "upload");
        f.environment.insert("FIXTURE_BLOCK_ARCHIVE_NUMBER", "1");
        QVERIFY(f.runs.queueManual({f.set.id}, true, &f.error));
        QProcess worker; f.start(worker);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(f.directory.filePath("blocked")), 30000);
        QVERIFY2(f.runs.requestControl(f.set.id, action, &f.error), qPrintable(f.error));
        QVERIFY(worker.waitForFinished(20000));
        QVERIFY(f.runs.load(&f.error));
        const auto stopped = *f.runs.find(f.set.id);
        QCOMPARE(stopped.status, action == "pause" ? QString("paused") : QString("cancelled"));
        QVERIFY(!QFile::exists(f.directory.filePath("remote") + stopped.remoteCopyPath + "/manifest.json"));
        f.environment.remove("FIXTURE_BLOCK_ARCHIVE_PHASE");
        if (action == "pause") {
            QVERIFY(f.runs.requestControl(f.set.id, "resume", &f.error));
            QVERIFY2(f.run(false), qPrintable(f.error));
            QCOMPARE(f.runs.find(f.set.id)->remoteCopyPath, stopped.remoteCopyPath);
        } else {
            QVERIFY2(f.run(), qPrintable(f.error));
            QVERIFY(f.runs.find(f.set.id)->remoteCopyPath != stopped.remoteCopyPath);
        }
        QVERIFY(f.runs.find(f.set.id)->result.manifestVerified);
    }

    void publishedManifestSurvivesLostResponseAndChangedSources()
    {
        ProcessFixture f; QVERIFY(f.prepareProcess());
        f.environment.insert("FIXTURE_BLOCK_ITEM", "manifest.json");
        QVERIFY(f.runs.queueManual({f.set.id}, true, &f.error));
        QProcess worker; f.start(worker);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(f.directory.filePath("blocked")), 30000);
        QVERIFY(f.runs.load(&f.error));
        const auto interrupted = *f.runs.find(f.set.id);
        worker.kill(); QVERIFY(worker.waitForFinished(10000));
        QVERIFY(write(f.directory.filePath("source/a.txt"), "changed after publication"));
        f.environment.remove("FIXTURE_BLOCK_ITEM");
        QVERIFY2(f.run(false), qPrintable(f.error));
        QCOMPARE(f.runs.find(f.set.id)->remoteCopyPath, interrupted.remoteCopyPath);
        QCOMPARE(f.archiveUploads(), 1);
        const auto manifest = QJsonDocument::fromJson(bytes(f.directory.filePath("remote") + interrupted.remoteCopyPath + "/manifest.json")).object();
        for (const auto &entry : manifest.value("entries").toArray())
            if (entry.toObject().value("restore") == "a.txt") QCOMPARE(entry.toObject().value("size").toInteger(), qint64(5));
    }

    void defaultWorkerResumesLegacyBeforeFreshBaseline()
    {
        ProcessFixture f; f.set.retention = 1; QVERIFY(f.prepareProcess());
        f.environment.insert("OMACUSTOS_INTERNAL_LEGACY_WORKER", "1");
        QVERIFY2(f.run(), qPrintable(f.error));
        const auto oldest = f.runs.find(f.set.id)->remoteCopyPath;
        f.environment.insert("FIXTURE_BLOCK_ARCHIVE_PHASE", "upload");
        f.environment.insert("FIXTURE_BLOCK_ARCHIVE_NUMBER", "2");
        QVERIFY(f.runs.queueManual({f.set.id}, true, &f.error));
        QProcess worker; f.start(worker);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(f.directory.filePath("blocked")), 30000);
        QVERIFY(f.runs.load(&f.error));
        const auto unfinished = f.runs.find(f.set.id)->remoteCopyPath;
        worker.kill(); QVERIFY(worker.waitForFinished(10000));
        f.environment.remove("FIXTURE_BLOCK_ARCHIVE_PHASE");
        f.environment.remove("OMACUSTOS_INTERNAL_LEGACY_WORKER");
        QVERIFY2(f.run(false), qPrintable(f.error));
        QCOMPARE(f.runs.find(f.set.id)->remoteCopyPath, unfinished);
        QVector<BackupEntry> entries; BackupManifestInfo info;
        QVERIFY(BackupManifest::load(f.directory.filePath("remote") + unfinished + "/manifest.json", &entries, &info, &f.error));
        QCOMPARE(info.version, 3);
        QVERIFY(QFile::exists(f.directory.filePath("remote") + oldest + "/manifest.json"));
        QVERIFY(f.runs.find(f.set.id)->incremental.value("cleanup_needs_attention").toBool());
        QVERIFY2(f.run(), qPrintable(f.error));
        QVERIFY(f.runs.find(f.set.id)->remoteCopyPath.contains("/sets/"));
        QVERIFY(QFile::exists(f.directory.filePath("remote") + oldest + "/manifest.json"));
        QVERIFY(QFile::exists(f.directory.filePath("remote") + unfinished + "/manifest.json"));
    }

    void sourceMutationDuringCaptureCannotPublish_data()
    {
        QTest::addColumn<bool>("replace");
        QTest::newRow("contents mutate") << false;
        QTest::newRow("pathname replaced") << true;
    }

    void sourceMutationDuringCaptureCannotPublish()
    {
        QFETCH(bool, replace);
        ProcessFixture f; QVERIFY(f.prepareProcess());
        const auto source = f.directory.filePath("source/a.txt");
        QVERIFY(write(source, QByteArray(4 * 1024 * 1024, 'a')));
        f.environment.insert("LD_PRELOAD", QStringLiteral(OMACUSTOS_QUOTA_FIXTURE));
        f.environment.insert("FIXTURE_MUTATE_DURING_STAGING", source);
        if (replace) f.environment.insert("FIXTURE_REPLACE_DURING_STAGING", "1");
        QVERIFY(!f.run());
        QVERIFY(!f.runs.find(f.set.id)->result.manifestVerified);
        QVERIFY(f.runs.find(f.set.id)->unfinished);
        QCOMPARE(f.archiveUploads(), 0);
        f.environment.remove("LD_PRELOAD");
        f.environment.remove("FIXTURE_MUTATE_DURING_STAGING");
        f.environment.remove("FIXTURE_REPLACE_DURING_STAGING");
        QVERIFY(f.runs.requestControl(f.set.id, "resume", &f.error));
        QVERIFY2(f.run(false), qPrintable(f.error));
    }

    void workerUploadFailureCannotPublish_data()
    {
        QTest::addColumn<QString>("failure");
        QTest::newRow("quota") << QString("quota");
        QTest::newRow("partial transfer") << QString("partial");
        QTest::newRow("corrupt upload") << QString("corrupt");
    }

    void workerUploadFailureCannotPublish()
    {
        QFETCH(QString, failure);
        ProcessFixture f; QVERIFY(f.prepareProcess());
        if (failure == "corrupt") f.environment.insert("FIXTURE_CORRUPT_ARCHIVE", "yes");
        else {
            f.environment.insert("FIXTURE_FAIL_ARCHIVE_NUMBER", "1");
            if (failure == "quota") f.environment.insert("FIXTURE_TRANSFER_ERROR", "Quota exceeded");
            else f.environment.insert("FIXTURE_PARTIAL_ARCHIVE", "yes");
        }
        QVERIFY(!f.run());
        const auto failed = *f.runs.find(f.set.id);
        QCOMPARE(failed.status, failure == "quota" ? QString("waiting") : QString("retrying"));
        QVERIFY(failed.unfinished);
        QVERIFY(!failed.result.manifestVerified);
        QVERIFY(!QFile::exists(f.directory.filePath("remote") + failed.remoteCopyPath + "/manifest.json"));
        f.environment.remove("FIXTURE_CORRUPT_ARCHIVE");
        f.environment.remove("FIXTURE_FAIL_ARCHIVE_NUMBER");
        f.environment.remove("FIXTURE_TRANSFER_ERROR");
        f.environment.remove("FIXTURE_PARTIAL_ARCHIVE");
        QVERIFY2(f.runs.requestControl(f.set.id, "resume", &f.error), qPrintable(f.error));
        QVERIFY2(f.run(false), qPrintable(f.error));
        QCOMPARE(f.runs.find(f.set.id)->remoteCopyPath, failed.remoteCopyPath);
        QVERIFY(f.runs.find(f.set.id)->result.manifestVerified);
    }

    void workerRejectsUnreadableRetainedManifest()
    {
        ProcessFixture f; QVERIFY(f.prepareProcess());
        QVERIFY2(f.run(), qPrintable(f.error));
        const auto saved = f.runs.find(f.set.id)->remoteCopyPath;
        const auto manifestPath = f.directory.filePath("remote") + saved + "/manifest.json";
        const auto original = bytes(manifestPath);
        QVERIFY(write(manifestPath, "invalid json"));
        QVERIFY(write(f.directory.filePath("source/a.txt"), "edit"));
        QVERIFY(!f.run());
        QVERIFY(!f.runs.find(f.set.id)->result.manifestVerified);
        QCOMPARE(bytes(manifestPath), QByteArray("invalid json"));
        const auto attempted = f.runs.find(f.set.id)->remoteCopyPath;
        QVERIFY(attempted != saved);
        QVERIFY(!QFile::exists(f.directory.filePath("remote") + attempted + "/manifest.json"));
        QVERIFY(write(manifestPath, original));
        QVERIFY2(f.runs.requestControl(f.set.id, "resume", &f.error), qPrintable(f.error));
        QVERIFY2(f.run(false), qPrintable(f.error));
        QCOMPARE(f.runs.find(f.set.id)->remoteCopyPath, attempted);
        QVERIFY(QFile::exists(manifestPath));
    }

    void mixedFormatUpgradeAndExactRetention_data()
    {
        QTest::addColumn<int>("version");
        QTest::newRow("v1 private files") << 1;
        QTest::newRow("v2 private files") << 2;
        QTest::newRow("v3 private archives") << 3;
    }

    void mixedFormatUpgradeAndExactRetention()
    {
        QFETCH(int, version);
        Fixture f; QVERIFY(f.prepare()); f.set.retention = 1;
        QVERIFY(write(f.directory.filePath("source/nested/manifest.json"), "ordinary source document, not backup metadata"));
        const QString oldParent = version == 1 ? "backups/old-computer/Documents" : "backups/old-computer/Old-name";
        const auto oldId = "20261019T100000000-old";
        const auto oldRoot = QString(oldParent) + '/' + oldId;
        BackupOptions options; options.freshCopy = true; options.boundedArchives = version == 3;
        options.stagingDirectory = f.set.stagingDirectory;
        options.stagingBudget = f.set.stagingBudget;
        options.continuationDirectory = f.directory.filePath("legacy-continuation");
        QString manifest;
        BackupEngine engine;
        BackupCopyMetadata metadata{"old-computer", f.set.id, "Old-name", oldId, QDateTime::currentDateTimeUtc().addDays(-1)};
        QVERIFY2(engine.backup(f.set.sourceDirectories, oldRoot, {}, metadata, f.provider, &manifest, &f.error, {}, nullptr, options), qPrintable(f.error));
        if (version == 1) {
            const auto path = f.directory.filePath("remote/") + oldRoot + "/manifest.json";
            const auto document = QJsonDocument::fromJson(bytes(path)).object();
            QVERIFY(write(path, QJsonDocument(QJsonObject{{"version", 1}, {"entries", document.value("entries")}}).toJson()));
        }
        IncrementalWriteRequest request; QVERIFY2(f.request(&request), qPrintable(f.error));
        QVERIFY(request.legacyRoots.contains(oldParent));
        const auto saved = IncrementalWorker::run(f.set, request, f.provider);
        QVERIFY2(saved.success(), saved.issues.isEmpty() ? "save failed" : qPrintable(saved.issues.first().reason));
        QCOMPARE(saved.savedFiles, 3);
        QVERIFY(saved.cleanupNeedsAttention);
        QVERIFY(QFile::exists(f.directory.filePath("remote/") + oldRoot + "/manifest.json"));
        BackupRestoreController browser(engine, &f.provider);
        browser.discover(f.set.remoteRoot, f.set.id, true, "Documents");
        QTRY_VERIFY_WITH_TIMEOUT(!browser.busy(), 10000);
        QCOMPARE(browser.copies().size(), 1); // The selected-set chooser skips historical formats.
        browser.selectCopy(0);
        QTRY_VERIFY_WITH_TIMEOUT(!browser.busy(), 10000);
        QCOMPARE(browser.currentCopyPath(), saved.latestSnapshotPath);
        QVERIFY2(browser.restoreEligible(), qPrintable(browser.browseError()));
        QCOMPARE(browser.entries().size(), 3);
        // General discovery and verification remain compatible with old copies.
        browser.discover(f.set.remoteRoot, {}, true, "Documents");
        QTRY_VERIFY_WITH_TIMEOUT(!browser.busy(), 10000);
        QCOMPARE(browser.copies().size(), 2);
        for (int index = 0; index < 2; ++index) {
            browser.selectCopy(index);
            QTRY_VERIFY_WITH_TIMEOUT(!browser.busy(), 10000);
            QVERIFY2(browser.restoreEligible(), qPrintable(browser.browseError()));
            QCOMPARE(browser.entries().size(), 3);
        }
        QJsonObject proposal; QString confirmation;
        QVERIFY(!IncrementalRepository::cleanup(request, f.provider, &proposal, &confirmation, &f.error));
        QCOMPARE(proposal.value("legacy").toArray().size(), 1);
        QCOMPARE(proposal.value("legacy").toArray().first().toObject().value("path").toString(), oldRoot);
        request.cleanupConfirmation = confirmation;
        QVERIFY2(IncrementalRepository::cleanup(request, f.provider, &proposal, &confirmation, &f.error), qPrintable(f.error));
        QVERIFY(!QFile::exists(f.directory.filePath("remote/") + oldRoot));
        RemoteCopy retained;
        QVERIFY2(IncrementalRestore::verifyCopy(f.provider, saved.latestSnapshotPath, request.scan.setId, &retained, &f.error), qPrintable(f.error));
        QCOMPARE(retained.entries.size(), 3);
        for (const auto &entry : retained.entries) QVERIFY(entry.archive.remotePath.startsWith(request.remoteSetRoot + "/objects/"));
        const auto newState = f.directory.filePath("recovered-owner");
        QVERIFY2(IncrementalWorker::takeover(f.set, newState, f.provider, true, &f.error), qPrintable(f.error));
        IncrementalWriteRequest recovered;
        QVERIFY2(IncrementalWorker::request(f.set, newState, uuid(), f.provider, &recovered, &f.error), qPrintable(f.error));
        QVERIFY(recovered.legacyRoots.contains(oldParent));
        QCOMPARE(recovered.ownershipEpoch, 2);
    }

    void missingAndSameSizeCorruptionThroughWorker_data()
    {
        QTest::addColumn<bool>("corrupt");
        QTest::newRow("missing object") << false;
        QTest::newRow("same-sized corruption") << true;
    }

    void missingAndSameSizeCorruptionThroughWorker()
    {
        QFETCH(bool, corrupt);
        ProcessFixture f; QVERIFY(f.prepareProcess());
        QVERIFY2(f.run(), qPrintable(f.error));
        const auto original = f.runs.find(f.set.id)->remoteCopyPath;
        const auto manifest = QJsonDocument::fromJson(bytes(f.directory.filePath("remote") + original + "/manifest.json")).object();
        const auto archive = manifest.value("archives").toArray().first().toObject();
        const auto object = f.directory.filePath("remote") + IncrementalWorker::root(f.set) + '/' + archive.value("remote").toString();
        if (corrupt) {
            QVERIFY(write(object, QByteArray(archive.value("size").toInteger(), '!')));
            QVERIFY2(f.run(), qPrintable(f.error));
            QCOMPARE(f.runs.find(f.set.id)->incremental.value("outcome").toString(), QString("unchanged"));
            FixtureEnvironment environment(f);
            IncrementalMaintenance maintenance(f.configPath);
            QSignalSpy completed(&maintenance, &IncrementalMaintenance::completed);
            QSignalSpy failed(&maintenance, &IncrementalMaintenance::failed);
            maintenance.checkIntegrity(f.set.id);
            QVERIFY(maintenance.busy());
            QCOMPARE(maintenance.property("integritySetId").toString(), f.set.id);
            QTRY_VERIFY_WITH_TIMEOUT(!maintenance.busy(), 60000);
            QCOMPARE(maintenance.property("integritySetId").toString(), QString());
            QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
            QCOMPARE(completed.size(), 1);
        } else QVERIFY(QFile::remove(object));
        QVERIFY2(f.run(), qPrintable(f.error));
        const auto repaired = *f.runs.find(f.set.id);
        QVERIFY(repaired.remoteCopyPath != original);
        QVERIFY(!repaired.incremental.value("affected_snapshots").toArray().isEmpty());
        FixtureEnvironment environment(f);
        QProcessRunner runner(f.directory.filePath("proton-fixture"));
        ProtonProvider provider(runner);
        RemoteCopy retained;
        QVERIFY(IncrementalRestore::verifyCopy(provider, repaired.remoteCopyPath, IncrementalWorker::setId(f.set), &retained, &f.error));
        BackupEngine engine;
        const auto restored = engine.restoreFiles({retained.entries, f.directory.filePath("repair-restore"), repaired.remoteCopyPath}, provider);
        QVERIFY2(restored.success, qPrintable(restored.error));
        QCOMPARE(bytes(f.directory.filePath("repair-restore/a.txt")), QByteArray("first"));
        RemoteCopy historical;
        QVERIFY(IncrementalRestore::verifyCopy(provider, original, IncrementalWorker::setId(f.set), &historical, &f.error));
        if (!corrupt) QVERIFY(!historical.unavailableItems.isEmpty());
        else QCOMPARE(bytes(object), QByteArray(archive.value("size").toInteger(), '!'));
    }

    void integrityActivityClearsAfterFailure()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        IncrementalMaintenance maintenance(directory.filePath("missing-settings.json"));
        QSignalSpy busy(&maintenance, &IncrementalMaintenance::busyChanged);
        QSignalSpy failed(&maintenance, &IncrementalMaintenance::failed);
        maintenance.checkIntegrity("documents-id");
        QVERIFY(maintenance.busy());
        QCOMPARE(maintenance.property("integritySetId").toString(), QString("documents-id"));
        maintenance.checkIntegrity("photos-id");
        QCOMPARE(maintenance.property("integritySetId").toString(), QString("documents-id"));
        QTRY_VERIFY_WITH_TIMEOUT(!maintenance.busy(), 1000);
        QCOMPARE(maintenance.property("integritySetId").toString(), QString());
        QCOMPARE(busy.size(), 2);
        QCOMPARE(failed.size(), 1);
    }

    void takeoverPreservesConcurrentConfiguration_data()
    {
        QTest::addColumn<QString>("change");
        QTest::newRow("editor save") << QString("edit");
        QTest::newRow("deleted set") << QString("delete");
        QTest::newRow("changed remote root") << QString("remote");
        QTest::newRow("configuration write locked") << QString("locked");
    }

    void takeoverPreservesConcurrentConfiguration()
    {
        QFETCH(QString, change);
        ProcessFixture f; f.set.readOnly = true; QVERIFY(f.prepareProcess());
        f.environment.insert("FIXTURE_BLOCK_ITEM", "repository.json");
        const auto release = f.directory.filePath("release");
        f.environment.insert("FIXTURE_RELEASE", release);
        FixtureEnvironment environment(f);
        BackupEngine engine;
        BackupSetController editor(engine, f.configPath);
        IncrementalMaintenance maintenance(f.configPath);
        const auto unblock = qScopeGuard([&] { write(release, "release"); });
        QSignalSpy completed(&maintenance, &IncrementalMaintenance::completed);
        QSignalSpy failed(&maintenance, &IncrementalMaintenance::failed);
        QSignalSpy editorFailed(&editor, &BackupSetController::failed);
        maintenance.takeover(f.set.id, true);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(f.directory.filePath("blocked")), 10000);
        if (change == "delete") editor.removeCurrentSet();
        else {
            editor.setCurrentName("Renamed while taking over");
            editor.setCurrentSources({f.directory.filePath("different-source")});
            editor.setCurrentExclusions({"*.tmp"});
            if (change == "remote") editor.setCurrentRemoteRoot("/my-files/different-root");
            QVERIFY(editor.save());
        }
        QVERIFY(editorFailed.isEmpty());
        QLockFile configLock(f.configPath + ".lock");
        if (change == "locked") QVERIFY(configLock.tryLock(0));
        QVERIFY(write(release, "release"));
        QTRY_VERIFY_WITH_TIMEOUT(!maintenance.busy(), 10000);
        BackupConfig saved;
        QVERIFY(BackupConfigStore(f.configPath).load(&saved, &f.error));
        if (change == "delete") QVERIFY(saved.sets.isEmpty());
        else {
            QCOMPARE(saved.sets.first().name, QString("Renamed while taking over"));
            QCOMPARE(saved.sets.first().sourceDirectories, QStringList{f.directory.filePath("different-source")});
            QCOMPARE(saved.sets.first().exclusions, QStringList{"*.tmp"});
            QCOMPARE(saved.sets.first().remoteRoot, change == "remote" ? QString("/my-files/different-root") : f.set.remoteRoot);
            QCOMPARE(saved.sets.first().readOnly, change != "edit");
        }
        QCOMPARE(completed.size(), change == "edit" ? 1 : 0);
        QCOMPARE(failed.size(), change == "edit" ? 0 : 1);
        if (change == "delete" || change == "remote")
            QVERIFY(failed.first().first().toString().contains("Local write authority was not granted"));
    }

    void settingsLossAndCopiedConfigurationNeedControllerTakeover()
    {
        ProcessFixture f; QVERIFY(f.prepareProcess());
        QVERIFY2(f.run(), qPrintable(f.error));
        const auto saved = f.runs.find(f.set.id)->remoteCopyPath;
        const auto copiedConfig = f.directory.filePath("copied/settings.json");
        QVERIFY(QDir().mkpath(QFileInfo(copiedConfig).absolutePath()));
        QVERIFY(QFile::copy(f.configPath, copiedConfig));
        BackupRunStore copiedRuns(f.directory.filePath("copied/omacustos-backup-runs.json"));
        const auto runCopied = [&](bool enqueue) {
            if (enqueue && !copiedRuns.queueManual({f.set.id}, true, &f.error)) return false;
            QProcess worker; worker.setProcessEnvironment(f.environment);
            worker.start(QStringLiteral(OMACUSTOS_WORKER_BINARY), {"--config", copiedConfig});
            if (!worker.waitForFinished(60000) || !copiedRuns.load(&f.error)) return false;
            return copiedRuns.find(f.set.id)->status == "success";
        };
        const auto transfers = bytes(f.directory.filePath("transfers.jsonl"));
        QVERIFY(!runCopied(true));
        QVERIFY(copiedRuns.find(f.set.id)->lastError.contains("read-only"));
        QCOMPARE(bytes(f.directory.filePath("transfers.jsonl")), transfers);
        QVERIFY(QDir().rename(f.directory.filePath("incremental-writers"), f.directory.filePath("writer-settings-offline")));
        QVERIFY(!f.run());
        QCOMPARE(bytes(f.directory.filePath("transfers.jsonl")), transfers);
        FixtureEnvironment environment(f);
        const auto takeOver = [&](const QString &path) {
            IncrementalMaintenance maintenance(path);
            QSignalSpy completed(&maintenance, &IncrementalMaintenance::completed);
            QSignalSpy failed(&maintenance, &IncrementalMaintenance::failed);
            maintenance.takeover(f.set.id, true);
            QElapsedTimer clock; clock.start();
            while (maintenance.busy() && clock.elapsed() < 60000) { QCoreApplication::processEvents(); QTest::qWait(10); }
            if (!failed.isEmpty()) f.error = failed.first().first().toString();
            return !maintenance.busy() && completed.size() == 1 && failed.isEmpty();
        };
        QVERIFY2(takeOver(f.configPath), qPrintable(f.error));
        QVERIFY(f.runs.requestControl(f.set.id, "resume", &f.error));
        QVERIFY2(f.run(false), qPrintable(f.error));
        QCOMPARE(f.runs.find(f.set.id)->remoteCopyPath, saved);
        QVERIFY2(takeOver(copiedConfig), qPrintable(f.error));
        QVERIFY(copiedRuns.requestControl(f.set.id, "resume", &f.error));
        QVERIFY2(runCopied(false), qPrintable(f.error));
        QCOMPARE(copiedRuns.find(f.set.id)->remoteCopyPath, saved);
        QVERIFY(!f.run()); // Former owner cannot write after the explicit handoff.
        QVERIFY(QFile::exists(f.directory.filePath("remote") + saved + "/manifest.json"));
    }

    void cleanupControllerRequiresConfirmationAndRestoresRetainedState()
    {
        ProcessFixture f; f.set.retention = 1; QVERIFY(f.prepareProcess());
        QVERIFY2(f.run(), qPrintable(f.error));
        const auto first = f.runs.find(f.set.id)->remoteCopyPath;
        QVERIFY(write(f.directory.filePath("source/a.txt"), "new"));
        QVERIFY2(f.run(), qPrintable(f.error));
        const auto latest = f.runs.find(f.set.id)->remoteCopyPath;
        FixtureEnvironment environment(f);
        IncrementalMaintenance maintenance(f.configPath);
        QSignalSpy ready(&maintenance, &IncrementalMaintenance::cleanupReady);
        QSignalSpy completed(&maintenance, &IncrementalMaintenance::completed);
        QSignalSpy failed(&maintenance, &IncrementalMaintenance::failed);
        maintenance.previewCleanup(f.set.id);
        QTRY_VERIFY_WITH_TIMEOUT(!maintenance.busy(), 60000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(ready.size(), 1);
        QVERIFY(QFile::exists(f.directory.filePath("remote") + first + "/manifest.json"));
        maintenance.cancelCleanup();
        QTRY_VERIFY_WITH_TIMEOUT(!maintenance.busy(), 60000);
        QCOMPARE(completed.size(), 1);
        QVERIFY(QFile::exists(f.directory.filePath("remote") + first + "/manifest.json"));
        maintenance.previewCleanup(f.set.id);
        QTRY_VERIFY_WITH_TIMEOUT(!maintenance.busy(), 60000);
        QCOMPARE(ready.size(), 2);
        maintenance.confirmCleanup();
        QTRY_VERIFY_WITH_TIMEOUT(!maintenance.busy(), 60000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(completed.size(), 2);
        QVERIFY(!QFile::exists(f.directory.filePath("remote") + first + "/manifest.json"));
        QVERIFY(QFile::exists(f.directory.filePath("remote") + latest + "/manifest.json"));
        QVERIFY(f.runs.load(&f.error));
        QVERIFY(!f.runs.find(f.set.id)->incremental.value("cleanup_needs_attention").toBool());
        QProcessRunner runner(f.directory.filePath("proton-fixture"));
        ProtonProvider provider(runner);
        RemoteCopy retained;
        QVERIFY2(IncrementalRestore::verifyCopy(provider, latest, IncrementalWorker::setId(f.set), &retained, &f.error), qPrintable(f.error));
        BackupEngine engine;
        const auto destination = f.directory.filePath("retained-restore");
        const auto restored = engine.restoreFiles({retained.entries, destination, latest}, provider);
        QVERIFY2(restored.success, qPrintable(restored.error));
        QCOMPARE(bytes(destination + "/a.txt"), QByteArray("new"));
        QCOMPARE(bytes(destination + "/b.txt"), QByteArray("unchanged"));
    }

    void protectedWriterAndInterruptedAuthorizedControllerCleanupRecover()
    {
        ProcessFixture f; f.set.retention = 1; QVERIFY(f.prepareProcess());
        QVERIFY2(f.run(), qPrintable(f.error));
        const auto first = f.runs.find(f.set.id)->remoteCopyPath;
        QVERIFY(write(f.directory.filePath("source/a.txt"), "second"));
        QVERIFY2(f.run(), qPrintable(f.error));
        const auto second = f.runs.find(f.set.id)->remoteCopyPath;
        QVERIFY(write(f.directory.filePath("source/b.txt"), "third"));
        f.environment.insert("FIXTURE_BLOCK_ARCHIVE_PHASE", "upload");
        f.environment.insert("FIXTURE_BLOCK_ARCHIVE_NUMBER", "3");
        QVERIFY(f.runs.queueManual({f.set.id}, true, &f.error));
        QProcess worker; f.start(worker);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(f.directory.filePath("blocked")), 30000);
        QVERIFY(f.runs.requestControl(f.set.id, "pause", &f.error));
        QVERIFY(worker.waitForFinished(20000));
        QVERIFY(f.runs.load(&f.error));
        QCOMPARE(f.runs.find(f.set.id)->status, QString("paused"));
        FixtureEnvironment environment(f);
        {
            IncrementalMaintenance maintenance(f.configPath);
            QSignalSpy ready(&maintenance, &IncrementalMaintenance::cleanupReady);
            QSignalSpy failed(&maintenance, &IncrementalMaintenance::failed);
            maintenance.previewCleanup(f.set.id);
            QTRY_VERIFY_WITH_TIMEOUT(!maintenance.busy(), 60000);
            QCOMPARE(ready.size(), 0);
            QCOMPARE(failed.size(), 1);
            QVERIFY(failed.first().first().toString().contains("Protected/resumable"));
        }
        QVERIFY(QFile::exists(f.directory.filePath("remote") + first + "/manifest.json"));
        QVERIFY(QFile::exists(f.directory.filePath("remote") + second + "/manifest.json"));
        f.environment.remove("FIXTURE_BLOCK_ARCHIVE_PHASE");
        QVERIFY(f.runs.requestControl(f.set.id, "resume", &f.error));
        QVERIFY2(f.run(false), qPrintable(f.error));
        const auto retained = f.runs.find(f.set.id)->remoteCopyPath;
        const auto manifest = QJsonDocument::fromJson(bytes(f.directory.filePath("remote") + retained + "/manifest.json")).object();
        const auto priorBlock = qgetenv("FIXTURE_BLOCK_ITEM");
        const auto restoreEnvironment = qScopeGuard([&] {
            if (priorBlock.isNull()) qunsetenv("FIXTURE_BLOCK_ITEM"); else qputenv("FIXTURE_BLOCK_ITEM", priorBlock);
        });
        QVERIFY(QFile::remove(f.directory.filePath("blocked")));
        {
            IncrementalMaintenance maintenance(f.configPath);
            QSignalSpy ready(&maintenance, &IncrementalMaintenance::cleanupReady);
            QSignalSpy failed(&maintenance, &IncrementalMaintenance::failed);
            maintenance.previewCleanup(f.set.id);
            QTRY_VERIFY_WITH_TIMEOUT(!maintenance.busy(), 60000);
            QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
            QCOMPARE(ready.size(), 1);
            qputenv("FIXTURE_BLOCK_ITEM", "authorized.json");
            maintenance.confirmCleanup();
            QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(f.directory.filePath("blocked")), 60000);
            // Simulate closing/crashing the UI after remote authorization was
            // written but its response was lost. Destruction stops the transfer.
        }
        qunsetenv("FIXTURE_BLOCK_ITEM");
        QVERIFY2(f.run(), qPrintable(f.error));
        QCOMPARE(f.runs.find(f.set.id)->remoteCopyPath, retained);
        QCOMPARE(f.runs.find(f.set.id)->incremental.value("outcome").toString(), QString("unchanged"));
        QVERIFY(!QFile::exists(f.directory.filePath("remote") + first + "/manifest.json"));
        QVERIFY(!QFile::exists(f.directory.filePath("remote") + second + "/manifest.json"));
        for (const auto &value : manifest.value("archives").toArray())
            QVERIFY(QFile::exists(f.directory.filePath("remote") + IncrementalWorker::root(f.set) + '/' + value.toObject().value("remote").toString()));
        QProcessRunner runner(f.directory.filePath("proton-fixture")); ProtonProvider provider(runner);
        RemoteCopy copy;
        QVERIFY(IncrementalRestore::verifyCopy(provider, retained, IncrementalWorker::setId(f.set), &copy, &f.error));
        BackupEngine engine;
        const auto destination = f.directory.filePath("recovered-retained");
        const auto restored = engine.restoreFiles({copy.entries, destination, retained}, provider);
        QVERIFY2(restored.success, qPrintable(restored.error));
        QCOMPARE(bytes(destination + "/a.txt"), QByteArray("second"));
        QCOMPARE(bytes(destination + "/b.txt"), QByteArray("third"));
    }
};
QTEST_GUILESS_MAIN(IncrementalWorkerTest)
#include "incrementalworker_test.moc"

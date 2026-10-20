#include "incrementalrepository.h"
#include "incrementalmanifest.h"
#include "localprovider.h"
#include "backuparchive.h"
#include "protonprovider.h"
#include "protonclifixture.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}
bool damage(const QString &path, const QByteArray &bytes)
{
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner) && write(path, bytes);
}
QJsonObject read(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

class FaultProvider : public BackupProvider {
public:
    explicit FaultProvider(const QString &root) : root(root), local(root) {}
    QString root;
    LocalProvider local;
    QStringList uploads, downloads;
    QString failUpload, failLookup;
    bool lostUploadResponse = false;
    bool sizeOnly = false;
    bool corruptUpload = false;
    int destructiveCalls = 0;
    bool upload(const QString &, const QString &, QString *) override { ++destructiveCalls; return false; }
    bool uploadImmutable(const QString &source, const QString &remote, QString *error) override
    {
        uploads.append(remote);
        if (!failUpload.isEmpty() && remote.contains(failUpload) && !lostUploadResponse) {
            if (error) *error = "Injected upload quota/network error";
            return false;
        }
        const bool success = local.uploadImmutable(source, remote, error);
        if (success && corruptUpload && remote.endsWith(".tar.gz") && !damage(QDir(root).filePath(remote), "corrupted")) {
            if (error) *error = "Fault injection could not damage the remote object.";
            return false;
        }
        if (success && !failUpload.isEmpty() && remote.contains(failUpload) && lostUploadResponse) {
            if (error) *error = "Injected lost upload response";
            return false;
        }
        return success;
    }
    RemoteLookup lookup(const QString &remote, QString *error) override
    {
        if (!failLookup.isEmpty() && remote.contains(failLookup)) {
            if (error) *error = "Network/authentication uncertainty";
            return RemoteLookup::Unknown;
        }
        return local.lookup(remote, error);
    }
    bool trustworthyContentChecksums() const override { return !sizeOnly; }
    bool ensureDirectory(const QString &remote, QString *error) override { return local.ensureDirectory(remote, error); }
    bool download(const QString &remote, const QString &destination, QString *error) override
    {
        downloads.append(remote);
        return local.download(remote, destination, error);
    }
    bool inspect(const QString &remote, RemoteFile *file, QString *error) override
    {
        const bool success = local.inspect(remote, file, error);
        if (sizeOnly) file->checksum.clear();
        return success;
    }
    bool list(const QString &remote, QVector<RemoteItem> *items, QString *error) override { return local.list(remote, items, error); }
    bool trash(const QString &, QString *) override { ++destructiveCalls; return false; }
    bool permanentlyDelete(const QString &, QString *) override { ++destructiveCalls; return false; }
    int objectUploads() const { return uploads.filter(QRegularExpression("\\.tar\\.gz$")).size(); }
    int objectDownloads() const { return downloads.filter(QRegularExpression("\\.tar\\.gz$")).size(); }
};

struct Fixture {
    QTemporaryDir directory{QDir::current().filePath("incremental-fixture-XXXXXX")};
    FaultProvider provider{directory.filePath("cloud")};
    IncrementalWriteRequest request;
    Fixture()
    {
        QDir().mkpath(directory.filePath("cloud"));
        QDir().mkpath(directory.filePath("sources"));
        request.scan.setId = uuid();
        request.remoteSetRoot = "sets/" + request.scan.setId;
        request.computerId = uuid();
        request.originComputerId = request.computerId;
        request.ownershipEpoch = 1;
        request.stateDirectory = directory.filePath("state");
        request.stagingDirectory = directory.filePath("staging");
        request.stagingBudget = 256 * 1024;
        request.archiveTargetBytes = 32 * 1024;
        request.scan.checkedAt = QDateTime::currentDateTimeUtc();
        next();
    }
    void next()
    {
        request.operationId = uuid();
        request.snapshotId = uuid();
        request.scan.checkedAt = request.scan.checkedAt.addSecs(1);
        request.checkpoint = {};
    }
    bool source(const QString &name, const QByteArray &bytes)
    {
        const QString path = directory.filePath("sources/" + name);
        if (!write(path, bytes)) return false;
        bool selected = false;
        for (const auto &source : request.scan.sources) selected = selected || source.sourcePath == path;
        if (!selected) request.scan.sources.append({path, name});
        return true;
    }
    bool initialize(QString *error) { return IncrementalRepository::initialize(request, provider, true, error); }
    QString cloud(const QString &relative) const { return directory.filePath("cloud/" + request.remoteSetRoot + '/' + relative); }
    QJsonObject snapshot(const QString &id) const { return read(cloud("snapshots/" + id + "/manifest.json")); }
    QStringList objectNames() const { return QDir(cloud("objects")).entryList({"*.tar.gz"}, QDir::Files, QDir::Name); }
    QStringList snapshotNames() const { return QDir(cloud("snapshots")).entryList(QDir::Dirs | QDir::NoDotAndDotDot); }
};

QString errors(const IncrementalWriteResult &result)
{
    QStringList reasons;
    for (const auto &issue : result.issues) reasons.append(issue.phase + ": " + issue.reason);
    return reasons.join('\n');
}
}

class IncrementalRepositoryTest : public QObject {
    Q_OBJECT
private slots:
    void protonAdapterEndToEndWithoutCloud()
    {
        Fixture fixture;
        FilesystemRunner runner;
        ProtonProvider provider(runner);
        QVERIFY(QDir().mkpath(runner.remoteFile("/my-files")));
        fixture.request.remoteSetRoot = "/my-files/" + fixture.request.remoteSetRoot;
        QVERIFY(fixture.source("a", "alpha"));
        QString error;
        QVERIFY2(IncrementalRepository::initialize(fixture.request, provider, true, &error), qPrintable(error));
        const auto first = IncrementalRepository::execute(fixture.request, provider);
        QVERIFY2(first.success(), qPrintable(errors(first)));
        const auto manifest = read(runner.remoteFile(first.manifestPath));
        QVERIFY(IncrementalManifest::validate(QJsonDocument(manifest).toJson(), fixture.request.scan.setId, fixture.request.snapshotId));
        fixture.next();
        const auto check = IncrementalRepository::execute(fixture.request, provider);
        QVERIFY2(check.success(), qPrintable(errors(check)));
        QCOMPARE(check.state, IncrementalWriteResult::State::Unchanged);
        for (const auto &call : runner.calls) {
            if (call.value(1) == "upload") {
                QVERIFY(!call.contains("replace"));
                QVERIFY(!call.contains("create-new-revision"));
                QVERIFY(!call.contains("rename"));
            }
        }
        QVERIFY(runner.trashedPaths.isEmpty());
    }

    void baselineEditAndUnchanged()
    {
        Fixture fixture;
        QString error;
        QVERIFY(fixture.source("a", "alpha"));
        QVERIFY(fixture.source("b", "beta"));
        QVERIFY2(fixture.initialize(&error), qPrintable(error));
        const auto first = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(first.success(), qPrintable(errors(first)));
        QVERIFY(first.manifestCommitted);
        QCOMPARE(first.state, IncrementalWriteResult::State::Saved);
        QCOMPARE(fixture.provider.objectUploads(), 1);
        QCOMPARE(fixture.provider.objectDownloads(), 1); // Full verification, even on size-only providers.
        const auto original = fixture.snapshot(fixture.request.snapshotId);
        QVERIFY(IncrementalManifest::validate(QJsonDocument(original).toJson(), fixture.request.scan.setId, fixture.request.snapshotId));
        QCOMPARE(original.value("entries").toArray().size(), 2);
        const auto originalObjects = fixture.objectNames();
        fixture.next();
        QVERIFY(fixture.source("a", "edited"));
        const auto edit = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(edit.success(), qPrintable(errors(edit)));
        QCOMPARE(fixture.provider.objectUploads(), 2);
        const auto changed = fixture.snapshot(fixture.request.snapshotId);
        QCOMPARE(changed.value("archives").toArray().size(), 2);
        QVERIFY(QFileInfo::exists(fixture.cloud("objects/" + originalObjects[0])));
        for (const auto &value : changed.value("archives").toArray())
            if (value.toObject().value("id") == original.value("archives").toArray()[0].toObject().value("id"))
                QCOMPARE(value, original.value("archives").toArray()[0]); // Unchanged full inventory, including old a.
        fixture.next();
        const int downloads = fixture.provider.objectDownloads();
        const auto unchanged = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(unchanged.success(), qPrintable(errors(unchanged)));
        QCOMPARE(unchanged.state, IncrementalWriteResult::State::Unchanged);
        QVERIFY(!unchanged.manifestCommitted);
        QCOMPARE(fixture.snapshotNames().size(), 2);
        QCOMPARE(fixture.provider.objectUploads(), 2);
        QCOMPARE(fixture.provider.objectDownloads(), downloads);
        QCOMPARE(unchanged.lastSavedChange, edit.lastSavedChange);
        QCOMPARE(unchanged.lastSuccessfulCheck, fixture.request.scan.checkedAt);
        fixture.request.scan.checkedAt = fixture.request.scan.checkedAt.addSecs(1);
        const auto recoveredCheck = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(recoveredCheck.success(), qPrintable(errors(recoveredCheck)));
        QCOMPARE(recoveredCheck.lastSuccessfulCheck, unchanged.lastSuccessfulCheck); // Idempotent terminal operation.
        QCOMPARE(fixture.provider.destructiveCalls, 0);
    }

    void interruptionRecovery_data()
    {
        QTest::addColumn<QString>("phase");
        for (const auto &phase : {"planned", "prepared-sources", "prepared-local", "protected", "uploaded",
                "object-verified", "verified-remote", "checkpointed", "publication-prepared"})
            QTest::newRow(phase) << QString(phase);
    }

    void interruptionRecovery()
    {
        QFETCH(QString, phase);
        Fixture fixture;
        QString error;
        QVERIFY(fixture.source("a", "alpha"));
        QVERIFY(fixture.source("b", "beta"));
        QVERIFY2(fixture.initialize(&error), qPrintable(error));
        bool interrupted = false;
        fixture.request.checkpoint = [&](const QString &current) {
            if (current == phase) { interrupted = true; return IncrementalControl::Pause; }
            return IncrementalControl::Continue;
        };
        const auto paused = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY(interrupted);
        QCOMPARE(paused.state, IncrementalWriteResult::State::Paused);
        QVERIFY(fixture.snapshot(fixture.request.snapshotId).isEmpty());
        const int uploads = fixture.provider.objectUploads();
        // Reconstruct all evidence and object mappings after losing local journals.
        QVERIFY(QDir(fixture.request.stateDirectory).removeRecursively());
        fixture.request.checkpoint = {};
        const auto resumed = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(resumed.success(), qPrintable(errors(resumed)));
        QVERIFY(!fixture.snapshot(fixture.request.snapshotId).isEmpty());
        if (uploads > 0) QCOMPARE(fixture.provider.objectUploads(), uploads);
        else QCOMPARE(fixture.provider.objectUploads(), 1);
        QCOMPARE(fixture.provider.destructiveCalls, 0);
    }

    void lostResponseRecovery_data()
    {
        QTest::addColumn<QString>("target");
        QTest::newRow("remote identity checkpoint") << QString("/identity.json");
        QTest::newRow("archive upload") << QString(".tar.gz");
        QTest::newRow("remote verified checkpoint") << QString("/verified-");
        QTest::newRow("snapshot publication") << QString("/manifest.json");
        QTest::newRow("success checkpoint") << QString("/complete.json");
    }

    void lostResponseRecovery()
    {
        QFETCH(QString, target);
        Fixture fixture;
        QString error;
        QVERIFY(fixture.source("a", "alpha"));
        QVERIFY(fixture.initialize(&error));
        fixture.provider.failUpload = target;
        fixture.provider.lostUploadResponse = true;
        const auto failed = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY(!failed.success());
        const int initialUploads = target == "/identity.json" ? 0 : 1;
        QCOMPARE(fixture.provider.objectUploads(), initialUploads);
        fixture.provider.failUpload.clear();
        const auto resumed = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(resumed.success(), qPrintable(errors(resumed)));
        QCOMPARE(fixture.provider.objectUploads(), 1);
        QCOMPARE(fixture.snapshotNames().size(), 1);
        QCOMPARE(fixture.provider.destructiveCalls, 0);
    }

    void identityFailureCanResumeOrCancel_data()
    {
        QTest::addColumn<bool>("cancel");
        QTest::newRow("resume after failed identity upload") << false;
        QTest::newRow("cancel after failed identity upload") << true;
    }

    void identityFailureCanResumeOrCancel()
    {
        QFETCH(bool, cancel);
        Fixture fixture;
        QString error;
        QVERIFY(fixture.source("a", "alpha"));
        QVERIFY(fixture.initialize(&error));
        fixture.provider.failUpload = "/identity.json";
        QVERIFY(!IncrementalRepository::execute(fixture.request, fixture.provider).success());
        QCOMPARE(fixture.provider.objectUploads(), 0);
        fixture.provider.failUpload.clear();
        fixture.request.scan.checkedAt = fixture.request.scan.checkedAt.addSecs(60); // Each real CLI invocation has a new check time.
        if (cancel) fixture.request.checkpoint = [](const QString &) { return IncrementalControl::Cancel; };
        const auto recovered = IncrementalRepository::execute(fixture.request, fixture.provider);
        if (cancel) QCOMPARE(recovered.state, IncrementalWriteResult::State::Cancelled);
        else QVERIFY2(recovered.success(), qPrintable(errors(recovered)));
        QCOMPARE(fixture.provider.objectUploads(), cancel ? 0 : 1);
    }

    void malformedCompletionRecordsFailClosed_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto &scenario : {"missing snapshot", "missing saved time", "invalid saved time", "extra field",
                "foreign snapshot", "wrong saved time", "regressing check time"})
            QTest::newRow(scenario) << QString(scenario);
    }

    void malformedCompletionRecordsFailClosed()
    {
        QFETCH(QString, scenario);
        Fixture fixture;
        QString error;
        QVERIFY(fixture.source("a", "alpha"));
        QVERIFY(fixture.initialize(&error));
        QVERIFY(IncrementalRepository::execute(fixture.request, fixture.provider).success());
        fixture.next();
        QVERIFY(IncrementalRepository::execute(fixture.request, fixture.provider).success());
        const QString path = fixture.cloud("operations/" + fixture.request.operationId + "/complete.json");
        auto completion = read(path);
        if (scenario == "missing snapshot") completion.remove("snapshot_id");
        if (scenario == "missing saved time") completion.remove("last_saved_change");
        if (scenario == "invalid saved time") completion["last_saved_change"] = "invalid";
        if (scenario == "extra field") completion["unrecognized"] = true;
        if (scenario == "foreign snapshot") completion["snapshot_id"] = uuid();
        if (scenario == "wrong saved time") completion["last_saved_change"] = fixture.request.scan.checkedAt.addSecs(-100).toString(Qt::ISODateWithMs);
        if (scenario == "regressing check time") completion["checked_at"] = fixture.request.scan.checkedAt.addSecs(-100).toString(Qt::ISODateWithMs);
        QVERIFY(damage(path, QJsonDocument(completion).toJson()));
        QVERIFY(!IncrementalRepository::execute(fixture.request, fixture.provider).success());
        fixture.next();
        QVERIFY(!IncrementalRepository::execute(fixture.request, fixture.provider).success());
        QCOMPARE(fixture.provider.objectUploads(), 1);
        QCOMPARE(fixture.provider.destructiveCalls, 0);
    }

    void loweredBudgetBlocksMultiMemberRecoveryDownload()
    {
        Fixture fixture;
        QString error;
        QVERIFY(fixture.source("a", "alpha"));
        QVERIFY(fixture.source("b", "beta"));
        QVERIFY(fixture.initialize(&error));
        fixture.request.checkpoint = [](const QString &phase) {
            return phase == "uploaded" ? IncrementalControl::Pause : IncrementalControl::Continue;
        };
        QCOMPARE(IncrementalRepository::execute(fixture.request, fixture.provider).state, IncrementalWriteResult::State::Paused);
        fixture.request.checkpoint = {};
        fixture.request.stagingBudget = 32;
        const int downloads = fixture.provider.objectDownloads();
        QCOMPARE(IncrementalRepository::execute(fixture.request, fixture.provider).state, IncrementalWriteResult::State::Waiting);
        QCOMPARE(fixture.provider.objectDownloads(), downloads);
        fixture.request.stagingBudget = 256 * 1024;
        QVERIFY(IncrementalRepository::execute(fixture.request, fixture.provider).success());
        QCOMPARE(fixture.provider.objectUploads(), 1);
    }

    void sourceChangesAndPublicationFreeze()
    {
        for (const auto &phase : {QString("prepared-sources"), QString("uploaded"), QString("publication-prepared")}) {
            Fixture fixture;
            QString error;
            QVERIFY(fixture.source("a", "original"));
            QVERIFY(fixture.initialize(&error));
            fixture.request.checkpoint = [&](const QString &current) {
                return current == phase ? IncrementalControl::Pause : IncrementalControl::Continue;
            };
            QCOMPARE(IncrementalRepository::execute(fixture.request, fixture.provider).state, IncrementalWriteResult::State::Paused);
            QVERIFY(fixture.source("a", "changed"));
            fixture.request.checkpoint = {};
            const auto resumed = IncrementalRepository::execute(fixture.request, fixture.provider);
            QVERIFY2(resumed.success(), qPrintable(errors(resumed)));
            const auto entry = fixture.snapshot(fixture.request.snapshotId).value("entries").toArray()[0].toObject();
            const QByteArray expected = phase == "publication-prepared" ? "original" : "changed";
            QCOMPARE(entry.value("sha256").toString(), QString::fromLatin1(QCryptographicHash::hash(expected, QCryptographicHash::Sha256).toHex()));
        }
    }

    void changedSincePlanningIsNotUploaded()
    {
        Fixture fixture;
        QString error;
        QVERIFY(fixture.source("a", "original"));
        QVERIFY(fixture.initialize(&error));
        bool changed = false;
        fixture.request.checkpoint = [&](const QString &phase) {
            if (phase == "planned") changed = fixture.source("a", "changed!");
            return IncrementalControl::Continue;
        };
        const auto failed = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY(changed);
        QCOMPARE(failed.state, IncrementalWriteResult::State::Failed);
        QCOMPARE(fixture.provider.objectUploads(), 0);
        QVERIFY(fixture.snapshotNames().isEmpty());
    }

    void repairedObjectsDoNotRepairHistory_data()
    {
        QTest::addColumn<bool>("missing");
        QTest::newRow("missing") << true;
        QTest::newRow("same-sized corruption with trustworthy hash") << false;
    }

    void repairedObjectsDoNotRepairHistory()
    {
        QFETCH(bool, missing);
        Fixture fixture;
        QString error;
        QVERIFY(fixture.source("a", "alpha"));
        QVERIFY(fixture.source("b", "beta"));
        QVERIFY(fixture.initialize(&error));
        const auto first = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(first.success(), qPrintable(errors(first)));
        const QString oldSnapshotId = fixture.request.snapshotId;
        const auto oldSnapshot = fixture.snapshot(oldSnapshotId);
        const QString object = fixture.cloud(oldSnapshot.value("archives").toArray()[0].toObject().value("remote").toString());
        if (missing) QVERIFY(QFile::remove(object));
        else QVERIFY(damage(object, QByteArray(QFileInfo(object).size(), 'x')));
        fixture.next();
        const auto repaired = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(repaired.success(), qPrintable(errors(repaired)));
        QCOMPARE(repaired.state, IncrementalWriteResult::State::Saved);
        QCOMPARE(repaired.affectedSnapshots, QStringList{oldSnapshotId});
        QCOMPARE(repaired.damagedObjects.size(), 1);
        QCOMPARE(fixture.snapshot(oldSnapshotId), oldSnapshot); // No historical rewrite claim.
        QVERIFY(fixture.snapshot(fixture.request.snapshotId).value("archives").toArray()[0].toObject().value("id")
            != oldSnapshot.value("archives").toArray()[0].toObject().value("id"));
        QCOMPARE(fixture.provider.destructiveCalls, 0);
    }

    void sizeOnlyDailyChecksExposeDelayedDetection()
    {
        Fixture fixture;
        fixture.provider.sizeOnly = true;
        QString error;
        QVERIFY(fixture.source("a", "alpha"));
        QVERIFY(fixture.initialize(&error));
        QVERIFY(IncrementalRepository::execute(fixture.request, fixture.provider).success());
        const auto archive = fixture.snapshot(fixture.request.snapshotId).value("archives").toArray()[0].toObject();
        const QString path = fixture.cloud(archive.value("remote").toString());
        QVERIFY(damage(path, QByteArray(QFileInfo(path).size(), 'x')));
        const int downloads = fixture.provider.objectDownloads();
        fixture.next();
        const auto check = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(check.success(), qPrintable(errors(check)));
        QCOMPARE(check.state, IncrementalWriteResult::State::Unchanged);
        QCOMPARE(fixture.provider.objectDownloads(), downloads);
        QVERIFY(!BackupArchiveIO::verify(path, archive.value("size").toInteger(), QByteArray::fromHex(archive.value("sha256").toString().toLatin1())));
    }

    void networkUncertaintyDoesNotBecomeAbsence()
    {
        Fixture fixture;
        QString error;
        QVERIFY(fixture.source("a", "alpha"));
        QVERIFY(fixture.initialize(&error));
        QVERIFY(IncrementalRepository::execute(fixture.request, fixture.provider).success());
        fixture.next();
        fixture.provider.failLookup = ".tar.gz";
        const auto failed = IncrementalRepository::execute(fixture.request, fixture.provider);
        QCOMPARE(failed.state, IncrementalWriteResult::State::Retrying);
        QCOMPARE(fixture.provider.objectUploads(), 1);
        QCOMPARE(fixture.snapshotNames().size(), 1);
        fixture.provider.failLookup.clear();
        QVERIFY(IncrementalRepository::execute(fixture.request, fixture.provider).success());
        QCOMPARE(fixture.provider.destructiveCalls, 0);
    }

    void quotaAndCorruptUploadsCannotPublish_data()
    {
        QTest::addColumn<bool>("quota");
        QTest::newRow("quota") << true;
        QTest::newRow("corrupt upload") << false;
    }

    void quotaAndCorruptUploadsCannotPublish()
    {
        QFETCH(bool, quota);
        Fixture fixture;
        QString error;
        QVERIFY(fixture.source("a", "alpha"));
        QVERIFY(fixture.initialize(&error));
        if (quota) fixture.provider.failUpload = ".tar.gz";
        else fixture.provider.corruptUpload = true;
        const auto failed = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY(!failed.success());
        if (quota) QCOMPARE(failed.state, IncrementalWriteResult::State::Waiting);
        QVERIFY(fixture.snapshot(fixture.request.snapshotId).isEmpty());
        fixture.provider.failUpload.clear();
        fixture.provider.corruptUpload = false;
        const auto resumed = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(resumed.success(), qPrintable(errors(resumed)));
        QCOMPARE(fixture.provider.destructiveCalls, 0);
    }

    void singleOwnerAndProtectedWork()
    {
        Fixture fixture;
        QString error;
        QVERIFY(fixture.source("a", "alpha"));
        QVERIFY(!IncrementalRepository::initialize(fixture.request, fixture.provider, false, &error));
        QVERIFY(fixture.initialize(&error));
        fixture.request.checkpoint = [](const QString &phase) {
            return phase == "uploaded" ? IncrementalControl::Pause : IncrementalControl::Continue;
        };
        QCOMPARE(IncrementalRepository::execute(fixture.request, fixture.provider).state, IncrementalWriteResult::State::Paused);
        const auto resumable = fixture.request;
        fixture.next();
        QVERIFY(!IncrementalRepository::execute(fixture.request, fixture.provider).success());
        fixture.request = resumable;
        fixture.request.checkpoint = [](const QString &) { return IncrementalControl::Cancel; };
        QCOMPARE(IncrementalRepository::execute(fixture.request, fixture.provider).state, IncrementalWriteResult::State::Cancelled);
        QCOMPARE(fixture.objectNames().size(), 1); // Cancellation records abandonment; never early deletes.
        fixture.next();
        auto foreign = fixture.request;
        foreign.computerId = uuid();
        QVERIFY(!IncrementalRepository::execute(foreign, fixture.provider).success());
        foreign = fixture.request;
        ++foreign.ownershipEpoch;
        QVERIFY(!IncrementalRepository::execute(foreign, fixture.provider).success());
        QVERIFY(IncrementalRepository::execute(fixture.request, fixture.provider).success());
        QCOMPARE(fixture.provider.destructiveCalls, 0);
    }

    void localExclusionAndOwnershipRecheck()
    {
        Fixture fixture;
        QString error;
        QVERIFY(fixture.source("a", "alpha"));
        QVERIFY(fixture.initialize(&error));
        bool excluded = false;
        fixture.request.checkpoint = [&](const QString &phase) {
            if (phase == "planned") {
                auto other = fixture.request;
                other.checkpoint = {};
                excluded = !IncrementalRepository::execute(other, fixture.provider).success();
                auto owner = read(fixture.cloud("repository.json"));
                owner["ownership_epoch"] = 2;
                write(fixture.cloud("repository.json"), QJsonDocument(owner).toJson());
            }
            return IncrementalControl::Continue;
        };
        QVERIFY(!IncrementalRepository::execute(fixture.request, fixture.provider).success());
        QVERIFY(excluded);
        QCOMPARE(fixture.provider.objectUploads(), 0);
    }

    void publishedSnapshotWinsLateCancel()
    {
        Fixture fixture;
        QString error;
        QVERIFY(fixture.source("a", "alpha"));
        QVERIFY(fixture.initialize(&error));
        fixture.request.checkpoint = [](const QString &phase) {
            return phase == "published" ? IncrementalControl::Cancel : IncrementalControl::Continue;
        };
        const auto saved = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(saved.success(), qPrintable(errors(saved)));
        QVERIFY(saved.manifestCommitted);
        QVERIFY(!QFileInfo::exists(fixture.cloud("operations/" + fixture.request.operationId + "/cancelled.json")));
    }

    void publicationIntentDoesNotOverrideCancel()
    {
        Fixture fixture;
        QString error;
        QVERIFY(fixture.source("a", "alpha"));
        QVERIFY(fixture.initialize(&error));
        fixture.request.checkpoint = [](const QString &phase) {
            return phase == "publication-prepared" ? IncrementalControl::Pause : IncrementalControl::Continue;
        };
        QCOMPARE(IncrementalRepository::execute(fixture.request, fixture.provider).state, IncrementalWriteResult::State::Paused);
        fixture.request.checkpoint = [](const QString &) { return IncrementalControl::Cancel; };
        QCOMPARE(IncrementalRepository::execute(fixture.request, fixture.provider).state, IncrementalWriteResult::State::Cancelled);
        QVERIFY(fixture.snapshot(fixture.request.snapshotId).isEmpty());
        QCOMPARE(fixture.provider.destructiveCalls, 0);
    }

    void emptyStateAndBoundedGroups()
    {
        Fixture fixture;
        QString error;
        QVERIFY(fixture.initialize(&error));
        QVERIFY(IncrementalRepository::execute(fixture.request, fixture.provider).success());
        QCOMPARE(fixture.provider.objectUploads(), 0);
        fixture.next();
        QVERIFY(fixture.source("a", QByteArray(40 * 1024, 'a')));
        QVERIFY(fixture.source("b", QByteArray(40 * 1024, 'b')));
        const auto grouped = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(grouped.success(), qPrintable(errors(grouped)));
        QCOMPARE(fixture.provider.objectUploads(), 2); // Files exceed target: kept whole, not split.
        fixture.next();
        fixture.request.scan.sources.clear();
        const auto emptied = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(emptied.success(), qPrintable(errors(emptied)));
        QCOMPARE(fixture.snapshot(fixture.request.snapshotId).value("entries").toArray().size(), 0);
        QCOMPARE(fixture.provider.objectUploads(), 2);
        QCOMPARE(fixture.provider.destructiveCalls, 0);
    }
};

QTEST_GUILESS_MAIN(IncrementalRepositoryTest)
#include "incrementalrepository_test.moc"

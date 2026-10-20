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
    QString failDownload;
    bool unknownRevision = false;
    std::function<void(const QString &)> afterDownload;
    std::function<void(const QString &)> afterInspect;
    bool lostUploadResponse = false;
    bool sizeOnly = false;
    bool corruptUpload = false;
    bool allowDeletion = false;
    bool failDelete = false;
    bool loseDeleteResponse = false;
    QStringList deletions;
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
        if (!failDownload.isEmpty() && remote.contains(failDownload)) {
            if (error) *error = "Network/authentication download failure";
            return false;
        }
        const bool success = local.download(remote, destination, error);
        if (success && afterDownload) afterDownload(remote);
        return success;
    }
    bool inspect(const QString &remote, RemoteFile *file, QString *error) override
    {
        const bool success = local.inspect(remote, file, error);
        if (sizeOnly) file->checksum.clear();
        if (unknownRevision) file->revision.clear();
        if (success && afterInspect) afterInspect(remote);
        return success;
    }
    bool list(const QString &remote, QVector<RemoteItem> *items, QString *error) override { return local.list(remote, items, error); }
    bool trash(const QString &, QString *) override { ++destructiveCalls; return false; }
    bool permanentlyDelete(const QString &, QString *) override { ++destructiveCalls; return false; }
    bool listComplete(const QString &path, QVector<RemoteItem> *items, QString *error) override { return local.listComplete(path, items, error); }
    bool deletionIdentity(const QString &path, QString *identity, QString *error) override { return local.deletionIdentity(path, identity, error); }
    bool trashExact(const QString &path, const QString &identity, QString *error) override
    {
        ++destructiveCalls;
        deletions.append("trash:" + path);
        return allowDeletion && local.trashExact(path, identity, error);
    }
    bool deleteExact(const QString &path, const QString &identity, QString *error) override
    {
        ++destructiveCalls;
        deletions.append("delete:" + path);
        if (failDelete) { if (error) *error = "Injected deletion failure"; return false; }
        const bool success = allowDeletion && local.deleteExact(path, identity, error);
        if (success && loseDeleteResponse) { if (error) *error = "Injected lost delete response"; return false; }
        return success;
    }
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
    bool cleanup(QJsonObject *proposal, QString *confirmation, QString *error)
    { return IncrementalRepository::cleanup(request, provider, proposal, confirmation, error); }
    bool legacy(const QString &name, int secondsAgo, bool incomplete = false)
    {
        request.legacyRoot = "legacy/Documents";
        const QString path = directory.filePath("cloud/" + request.legacyRoot + '/' + name);
        if (!QDir().mkpath(path)) return false;
        BackupManifestDraft draft;
        draft.metadata = {"computer", request.scan.setId, "Documents", name, request.scan.checkedAt.addSecs(-secondsAgo)};
        if (incomplete) { draft.expectedItems = {"missing"}; draft.failedItems = {"missing"}; }
        return BackupManifest::write(QDir(path).filePath("manifest.json"), draft);
    }
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
    void fullIntegrityDetectsHistoricalSameSizeDamage_data()
    {
        QTest::addColumn<bool>("checksums");
        QTest::newRow("size-only provider") << false;
        QTest::newRow("trustworthy SHA-256 provider") << true;
    }

    void fullIntegrityDetectsHistoricalSameSizeDamage()
    {
        QFETCH(bool, checksums);
        Fixture f;
        f.provider.sizeOnly = !checksums;
        QString error;
        QVERIFY(f.source("a", "original bytes"));
        QVERIFY(f.initialize(&error));
        QVERIFY(IncrementalRepository::execute(f.request, f.provider).success());
        const QString first = f.request.snapshotId;
        const QString object = f.objectNames().first();
        f.next();
        QVERIFY(f.source("b", "new data"));
        QVERIFY(IncrementalRepository::execute(f.request, f.provider).success());
        const QString second = f.request.snapshotId;
        const QString path = f.cloud("objects/" + object);
        const auto size = QFileInfo(path).size();
        QVERIFY(damage(path, QByteArray(size, 'x')));
        f.next();
        if (!checksums) {
            const auto daily = IncrementalRepository::execute(f.request, f.provider);
            QVERIFY2(daily.success(), qPrintable(errors(daily)));
            QVERIFY(daily.damagedObjects.isEmpty());
            QVERIFY(daily.integrity.value("daily_check").toString().contains("do not detect same-size"));
        }
        const auto report = IncrementalRepository::checkIntegrity(f.request, f.provider, true);
        QVERIFY2(!report.contains("error"), QJsonDocument(report).toJson().constData());
        QCOMPARE(report.value("status"), "damaged");
        QVERIFY(report.value("cleanup_blocked").toBool());
        QCOMPARE(report.value("damaged_objects").toArray().size(), 1);
        const auto affected = report.value("affected_snapshots").toArray();
        QVERIFY(affected.contains(first));
        QVERIFY(affected.contains(second));
        QCOMPARE(f.snapshotNames().size(), 2);
        QCOMPARE(QFileInfo(path).size(), size);
        QCOMPARE(f.provider.destructiveCalls, 0);
        f.request.retention = 1;
        QJsonObject proposal;
        QString confirmation;
        QVERIFY(!f.cleanup(&proposal, &confirmation, &error));
        QCOMPARE(f.provider.destructiveCalls, 0);
    }

    void integrityBudgetResumeAndCoverage()
    {
        Fixture f;
        QString error;
        f.request.batchFileLimit = 1;
        QVERIFY(f.source("a", "alpha"));
        QVERIFY(f.source("b", "beta"));
        QVERIFY(f.initialize(&error));
        QVERIFY(IncrementalRepository::execute(f.request, f.provider).success());
        f.request.integrityByteBudget = 1; // A whole archive may exceed a soft slice.
        f.provider.sizeOnly = true;
        f.provider.downloads.clear();
        auto report = IncrementalRepository::checkIntegrity(f.request, f.provider, true);
        QCOMPARE(report.value("status"), "in_progress");
        QCOMPARE(report.value("checked_objects").toInt(), 1);
        QCOMPARE(report.value("total_objects").toInt(), 2);
        QCOMPARE(report.value("remaining_objects").toInt(), 1);
        QCOMPARE(f.provider.objectDownloads(), 1);
        const QString cycle = report.value("cycle_id").toString();
        // Losing local cursor state cannot discard the remote checkpoint.
        f.request.stateDirectory = f.directory.filePath("replacement-state");
        f.next();
        report = IncrementalRepository::checkIntegrity(f.request, f.provider);
        QCOMPARE(report.value("status"), "complete");
        QCOMPARE(report.value("cycle_id").toString(), cycle);
        QCOMPARE(report.value("checked_objects").toInt(), 2);
        QCOMPARE(report.value("checked_bytes"), report.value("total_bytes"));
        QCOMPARE(f.provider.objectDownloads(), 2);
        QVERIFY(!report.value("cleanup_blocked").toBool());
        report = IncrementalRepository::checkIntegrity(f.request, f.provider);
        QCOMPARE(report.value("status"), "not_due");
        QCOMPARE(f.provider.objectDownloads(), 2);
        f.request.scan.checkedAt = f.request.scan.checkedAt.addDays(30);
        report = IncrementalRepository::checkIntegrity(f.request, f.provider);
        QCOMPARE(report.value("status"), "in_progress");
        QVERIFY(report.value("overdue").toBool());
        QVERIFY(report.value("cycle_id").toString() != cycle);
    }

    void integrityPauseCancelAndUnknownRevision()
    {
        Fixture f;
        QString error;
        QVERIFY(f.source("a", "alpha"));
        QVERIFY(f.initialize(&error));
        QVERIFY(IncrementalRepository::execute(f.request, f.provider).success());
        f.provider.sizeOnly = true;
        f.provider.unknownRevision = true;
        f.request.checkpoint = [](const QString &) { return IncrementalControl::Pause; };
        auto report = IncrementalRepository::checkIntegrity(f.request, f.provider, true);
        QCOMPARE(report.value("status"), "paused");
        const QString cycle = report.value("cycle_id").toString();
        QVERIFY(!IncrementalRepository::takeover(f.request, f.provider, uuid(), true, &error));
        f.request.checkpoint = [](const QString &) { return IncrementalControl::Cancel; };
        report = IncrementalRepository::checkIntegrity(f.request, f.provider);
        QCOMPARE(report.value("status"), "cancelled");
        f.next();
        report = IncrementalRepository::checkIntegrity(f.request, f.provider);
        QCOMPARE(report.value("status"), "complete");
        QVERIFY(report.value("cycle_id").toString() != cycle);
        QCOMPARE(report.value("checked_objects").toInt(), 1);
    }

    void integrityLostCheckpointResponse_data()
    {
        QTest::addColumn<bool>("lostResponse");
        QTest::newRow("upload failed") << false;
        QTest::newRow("uploaded but response lost") << true;
    }

    void integrityLostCheckpointResponse()
    {
        QFETCH(bool, lostResponse);
        Fixture f;
        QString error;
        QVERIFY(f.source("a", "alpha"));
        QVERIFY(f.initialize(&error));
        QVERIFY(IncrementalRepository::execute(f.request, f.provider).success());
        f.provider.sizeOnly = true;
        f.provider.downloads.clear();
        f.provider.failUpload = "/checks/";
        f.provider.lostUploadResponse = lostResponse;
        auto report = IncrementalRepository::checkIntegrity(f.request, f.provider, true);
        QCOMPARE(report.value("status"), "retrying");
        QVERIFY(report.value("cleanup_blocked").toBool());
        f.provider.failUpload.clear();
        f.next();
        report = IncrementalRepository::checkIntegrity(f.request, f.provider);
        QCOMPARE(report.value("status"), "complete");
        QCOMPARE(f.provider.objectDownloads(), lostResponse ? 1 : 2);
    }

    void integrityNetworkFailureAndRevisionChange()
    {
        Fixture f;
        QString error;
        QVERIFY(f.source("a", "alpha"));
        QVERIFY(f.initialize(&error));
        const auto saved = IncrementalRepository::execute(f.request, f.provider);
        QVERIFY(saved.success());
        f.provider.sizeOnly = true;
        f.provider.failDownload = ".tar.gz";
        auto report = IncrementalRepository::checkIntegrity(f.request, f.provider, true);
        QCOMPARE(report.value("status"), "retrying");
        QVERIFY(report.value("damaged_objects").toArray().isEmpty());
        f.provider.failDownload.clear();
        f.provider.afterDownload = [&](const QString &path) {
            if (path.endsWith(".tar.gz")) QVERIFY(damage(QDir(f.provider.root).filePath(path), QByteArray(QFileInfo(QDir(f.provider.root).filePath(path)).size(), 'x')));
        };
        report = IncrementalRepository::checkIntegrity(f.request, f.provider);
        QCOMPARE(report.value("status"), "retrying");
        QVERIFY(report.value("error").toString().contains("revision changed"));
        f.provider.afterDownload = {};
        report = IncrementalRepository::checkIntegrity(f.request, f.provider);
        QCOMPARE(report.value("status"), "damaged");
        f.next();
        const auto replacement = IncrementalRepository::execute(f.request, f.provider);
        QVERIFY(replacement.success());
        QVERIFY(replacement.lastSuccessfulCheck > saved.lastSuccessfulCheck);
        QVERIFY(replacement.lastSavedChange > saved.lastSavedChange);
        QVERIFY(replacement.cleanupNeedsAttention);
        QCOMPARE(replacement.integrity.value("status"), "damaged");
        QVERIFY(!replacement.affectedSnapshots.isEmpty());
        QCOMPARE(f.provider.destructiveCalls, 0);
    }

    void integrityReconcilesNewReachabilityAndProtectedUploads()
    {
        Fixture f;
        QString error;
        f.request.batchFileLimit = 1;
        QVERIFY(f.source("a", "alpha"));
        QVERIFY(f.initialize(&error));
        QVERIFY(IncrementalRepository::execute(f.request, f.provider).success());
        f.next();
        QVERIFY(f.source("b", "beta"));
        f.request.checkpoint = [](const QString &phase) { return phase == "uploaded" ? IncrementalControl::Pause : IncrementalControl::Continue; };
        QCOMPARE(IncrementalRepository::execute(f.request, f.provider).state, IncrementalWriteResult::State::Paused);
        f.request.checkpoint = {};
        auto report = IncrementalRepository::checkIntegrity(f.request, f.provider, true);
        QCOMPARE(report.value("status"), "complete");
        QCOMPARE(report.value("checked_objects").toInt(), 2); // Includes the unpublished prepared upload.
        QVERIFY(IncrementalRepository::execute(f.request, f.provider).success());
        f.request.checkpoint = [](const QString &) { return IncrementalControl::Pause; };
        report = IncrementalRepository::checkIntegrity(f.request, f.provider, true);
        QCOMPARE(report.value("status"), "paused");
        f.next();
        QVERIFY(f.source("c", "gamma"));
        f.request.checkpoint = [](const QString &phase) { return phase == "integrity-object" ? IncrementalControl::Pause : IncrementalControl::Continue; };
        QVERIFY(IncrementalRepository::execute(f.request, f.provider).success());
        f.request.checkpoint = {};
        report = IncrementalRepository::checkIntegrity(f.request, f.provider);
        QCOMPARE(report.value("status"), "complete");
        QCOMPARE(report.value("checked_objects").toInt(), 3);
    }

    void integrityTimeBudgetLedgerLossAndStagingWait()
    {
        Fixture f;
        QString error;
        f.request.batchFileLimit = 1;
        QVERIFY(f.source("a", "alpha"));
        QVERIFY(f.source("b", "beta"));
        QVERIFY(f.initialize(&error));
        QVERIFY(IncrementalRepository::execute(f.request, f.provider).success());
        f.request.integrityTimeBudgetMs = 1;
        f.provider.afterDownload = [](const QString &path) { if (path.endsWith(".tar.gz")) QTest::qSleep(2); };
        auto report = IncrementalRepository::checkIntegrity(f.request, f.provider, true);
        QCOMPARE(report.value("status"), "in_progress");
        QVERIFY(report.value("checked_objects").toInt() < 2);
        f.request.integrityTimeBudgetMs = 3600000;
        f.provider.afterDownload = {};
        report = IncrementalRepository::checkIntegrity(f.request, f.provider);
        QCOMPARE(report.value("status"), "complete");
        QVERIFY(QDir(f.cloud("integrity")).removeRecursively()); // Only this fixture's coverage ledger.
        report = IncrementalRepository::checkIntegrity(f.request, f.provider);
        QCOMPARE(report.value("status"), "complete");
        QCOMPARE(report.value("checked_objects").toInt(), 2);
        QVERIFY(!report.value("last_completed_at").toString().isEmpty());

        Fixture grouped;
        QVERIFY(grouped.source("a", "alpha"));
        QVERIFY(grouped.source("b", "beta"));
        QVERIFY(grouped.initialize(&error));
        QVERIFY(IncrementalRepository::execute(grouped.request, grouped.provider).success());
        grouped.request.stagingBudget = 1;
        report = IncrementalRepository::checkIntegrity(grouped.request, grouped.provider, true);
        QCOMPARE(report.value("status"), "waiting");
        QVERIFY(report.value("cleanup_blocked").toBool());
        grouped.request.stagingBudget = 256 * 1024;
        report = IncrementalRepository::checkIntegrity(grouped.request, grouped.provider);
        QCOMPARE(report.value("status"), "complete");
    }

    void integrityRejectsIncompleteOrUnboundCompletion_data()
    {
        QTest::addColumn<bool>("omitCoverage");
        QTest::newRow("omitted live worklist") << true;
        QTest::newRow("forged receipt binding") << false;
    }

    void integrityRejectsIncompleteOrUnboundCompletion()
    {
        QFETCH(bool, omitCoverage);
        Fixture f;
        QString error;
        QVERIFY(f.source("a", "alpha"));
        QVERIFY(f.initialize(&error));
        QVERIFY(IncrementalRepository::execute(f.request, f.provider).success());
        const auto report = IncrementalRepository::checkIntegrity(f.request, f.provider, true);
        QCOMPARE(report.value("status"), "complete");
        const QString path = f.cloud("integrity/" + report.value("cycle_id").toString() + "/complete.json");
        auto completion = read(path);
        if (omitCoverage) {
            completion["objects"] = QJsonArray{};
            completion["checked_objects"] = 0;
            completion["checked_bytes"] = 0;
        } else {
            auto objects = completion.value("objects").toArray();
            auto receipt = objects.first().toObject();
            receipt["binding"] = QString(64, '0');
            objects[0] = receipt;
            completion["objects"] = objects;
        }
        QVERIFY(damage(path, QJsonDocument(completion).toJson()));
        const auto retry = IncrementalRepository::checkIntegrity(f.request, f.provider);
        QCOMPARE(retry.value("status"), "retrying");
        QVERIFY(retry.value("cleanup_blocked").toBool());
        QCOMPARE(f.provider.destructiveCalls, 0);
    }

    void integrityRejectsConflictingOriginalReceipts()
    {
        Fixture f;
        QString error;
        QVERIFY(f.source("a", "alpha"));
        QVERIFY(f.initialize(&error));
        QVERIFY(IncrementalRepository::execute(f.request, f.provider).success());
        f.next();
        QVERIFY(f.source("b", "beta"));
        QVERIFY(IncrementalRepository::execute(f.request, f.provider).success());
        auto manifest = f.snapshot(f.request.snapshotId);
        auto archives = manifest.value("archives").toArray();
        for (int index = 0; index < archives.size(); ++index) {
            auto archive = archives[index].toObject();
            auto original = archive.value("original_verification").toObject();
            original["verified_at"] = f.request.scan.checkedAt.addMSecs(1).toUTC().toString(Qt::ISODateWithMs);
            archive["original_verification"] = original;
            archives[index] = archive;
        }
        manifest["archives"] = archives;
        QVERIFY(IncrementalManifest::validate(QJsonDocument(manifest).toJson(), f.request.scan.setId, f.request.snapshotId, &error));
        QVERIFY(damage(f.cloud("snapshots/" + f.request.snapshotId + "/manifest.json"), QJsonDocument(manifest).toJson()));
        const auto report = IncrementalRepository::checkIntegrity(f.request, f.provider, true);
        QCOMPARE(report.value("status"), "retrying");
        QVERIFY(report.value("error").toString().contains("original verification receipts"));
        QVERIFY(report.value("cleanup_blocked").toBool());
    }

    void knownDamageSurvivesCoverageCancellation_data()
    {
        QTest::addColumn<bool>("failedDamageUpload");
        QTest::newRow("cancel and pause new coverage") << false;
        QTest::newRow("damage checkpoint upload failure") << true;
    }

    void knownDamageSurvivesCoverageCancellation()
    {
        QFETCH(bool, failedDamageUpload);
        Fixture f;
        QString error;
        QVERIFY(f.source("a", "alpha"));
        QVERIFY(f.initialize(&error));
        QVERIFY(IncrementalRepository::execute(f.request, f.provider).success());
        const QString oldSnapshot = f.request.snapshotId;
        const QString object = f.objectNames().first();
        const QString path = f.cloud("objects/" + object);
        QVERIFY(damage(path, QByteArray(QFileInfo(path).size(), 'x')));
        f.provider.sizeOnly = true;
        if (failedDamageUpload) f.provider.failUpload = "integrity-damage/";
        auto report = IncrementalRepository::checkIntegrity(f.request, f.provider, true);
        QCOMPARE(report.value("status"), failedDamageUpload ? "retrying" : "damaged");
        QVERIFY(!report.value("damaged_objects").toArray().isEmpty());
        f.provider.failUpload.clear();
        f.request.checkpoint = [](const QString &) { return IncrementalControl::Cancel; };
        report = IncrementalRepository::checkIntegrity(f.request, f.provider);
        QCOMPARE(report.value("status"), "cancelled");
        f.next();
        f.request.integrityByteBudget = 1;
        f.request.checkpoint = [](const QString &phase) { return phase.startsWith("integrity-") ? IncrementalControl::Pause : IncrementalControl::Continue; };
        const auto replacement = IncrementalRepository::execute(f.request, f.provider);
        QVERIFY2(replacement.success(), qPrintable(errors(replacement)));
        QCOMPARE(replacement.state, IncrementalWriteResult::State::Saved);
        QVERIFY(replacement.affectedSnapshots.contains(oldSnapshot));
        QVERIFY(replacement.cleanupNeedsAttention);
        QCOMPARE(f.objectNames().size(), 2);
        f.provider.failLookup = "integrity-damage";
        f.next();
        QVERIFY(!IncrementalRepository::execute(f.request, f.provider).success());
        QCOMPARE(f.provider.destructiveCalls, 0);
    }

    void dailyCorruptionKnowledgeSurvivesFailedReplacement_data()
    {
        QTest::addColumn<bool>("ownerReadFailure");
        QTest::newRow("replacement upload failure") << false;
        QTest::newRow("owner read fails at corruption checkpoint") << true;
    }

    void dailyCorruptionKnowledgeSurvivesFailedReplacement()
    {
        QFETCH(bool, ownerReadFailure);
        Fixture f;
        QString error;
        QVERIFY(f.source("a", "alpha"));
        QVERIFY(f.initialize(&error));
        QVERIFY(IncrementalRepository::execute(f.request, f.provider).success());
        const QString oldSnapshot = f.request.snapshotId;
        const QString path = f.cloud("objects/" + f.objectNames().first());
        QVERIFY(damage(path, QByteArray(QFileInfo(path).size(), 'x')));
        f.next();
        if (ownerReadFailure) f.provider.afterInspect = [&](const QString &remote) {
            if (remote.endsWith(".tar.gz")) f.provider.failDownload = "repository.json";
        };
        else f.provider.failUpload = ".tar.gz";
        const auto failed = IncrementalRepository::execute(f.request, f.provider);
        QVERIFY(!failed.success());
        QVERIFY(!failed.damagedObjects.isEmpty());
        f.provider.failUpload.clear();
        f.provider.failDownload.clear();
        f.provider.afterInspect = {};
        f.provider.sizeOnly = true;
        f.request.checkpoint = [](const QString &phase) { return phase.startsWith("integrity-") ? IncrementalControl::Pause : IncrementalControl::Continue; };
        const auto resumed = IncrementalRepository::execute(f.request, f.provider);
        QVERIFY2(resumed.success(), qPrintable(errors(resumed)));
        QCOMPARE(resumed.state, IncrementalWriteResult::State::Saved);
        QVERIFY(resumed.affectedSnapshots.contains(oldSnapshot));
        QCOMPARE(f.objectNames().size(), 2);
    }

    void protonDeletionRequiresActualAncestry_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto &fault : {"short UID alias", "long UID alias", "listed parent mismatch", "resolved parent mismatch"})
            QTest::newRow(fault) << QString(fault);
    }

    void protonDeletionRequiresActualAncestry()
    {
        QFETCH(QString, fault);
        FilesystemRunner runner;
        ProtonProvider provider(runner);
        QVERIFY(QDir().mkpath(runner.remoteFile("/my-files/allowed")));
        QVERIFY(QDir().mkpath(runner.remoteFile("/my-files/outside")));
        QVERIFY(write(runner.remoteFile("/my-files/outside/victim"), "unrelated data"));
        QString path = "/my-files/outside/victim";
        if (fault.contains("UID alias")) {
            const int length = fault.startsWith("short") ? 22 : 88;
            const QString alias = QString(length, 'a') + '~' + QString(length, 'b');
            runner.globalAliases[alias] = "/my-files/outside";
            path = "/my-files/allowed/" + alias + "/victim";
            QCOMPARE(runner.remoteFile(path), runner.remoteFile("/my-files/outside/victim"));
        } else if (fault == "listed parent mismatch") runner.wrongListedParentName = "victim";
        else runner.wrongResolvedParentPath = path;
        QString identity, error;
        QVERIFY(!provider.deletionIdentity(path, &identity, &error));
        QVERIFY2(!error.isEmpty(), qPrintable(fault));
        QVector<RemoteItem> items;
        QVERIFY(!provider.listComplete(path, &items, &error));
        for (const auto &call : runner.calls) QVERIFY(call[1] != "trash" && call[1] != "delete");
        QVERIFY(QFileInfo::exists(runner.remoteFile("/my-files/outside/victim")));
    }

    void cleanupProposalPublicationFailureIsRecoverable_data()
    {
        QTest::addColumn<bool>("lostResponse");
        QTest::newRow("failed before publication") << false;
        QTest::newRow("lost publication response") << true;
    }

    void cleanupProposalPublicationFailureIsRecoverable()
    {
        QFETCH(bool, lostResponse);
        Fixture fixture;
        QString error, confirmation;
        QJsonObject proposal;
        QVERIFY(fixture.source("a", "old"));
        QVERIFY(fixture.initialize(&error));
        QVERIFY(IncrementalRepository::execute(fixture.request, fixture.provider).success());
        const QString old = fixture.request.snapshotId;
        fixture.next();
        QVERIFY(fixture.source("a", "new"));
        fixture.request.retention = 1;
        fixture.provider.failUpload = "proposal.json";
        fixture.provider.lostUploadResponse = lostResponse;
        const auto saved = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(saved.success(), qPrintable(errors(saved)));
        QVERIFY(saved.cleanupNeedsAttention);
        QCOMPARE(fixture.provider.destructiveCalls, 0);
        QVERIFY(!fixture.snapshot(old).isEmpty());
        QVERIFY(QDir(fixture.request.stateDirectory).removeRecursively());
        fixture.provider.failUpload.clear();
        fixture.provider.lostUploadResponse = false;
        if (!lostResponse) {
            auto writer = fixture.request;
            writer.retention = 0;
            const auto recovered = IncrementalRepository::execute(writer, fixture.provider);
            QVERIFY2(recovered.success(), qPrintable(errors(recovered)));
        }
        QVERIFY(!fixture.cleanup(&proposal, &confirmation, &error)); // Fresh exact authorization is still required.
        QVERIFY2(!confirmation.isEmpty(), qPrintable(error));
        fixture.request.cleanupConfirmation = confirmation;
        fixture.provider.allowDeletion = true;
        QVERIFY2(fixture.cleanup(&proposal, &confirmation, &error), qPrintable(error));
        QVERIFY(fixture.snapshot(old).isEmpty());
        fixture.next();
        const auto resumed = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(resumed.success(), qPrintable(errors(resumed)));
        QCOMPARE(resumed.state, IncrementalWriteResult::State::Unchanged);
    }

    void protonCleanupRecovery_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto &fault : {"trash", "delete", "item failure", "duplicate trash name"})
            QTest::newRow(fault) << QString(fault);
    }

    void protonCleanupRecovery()
    {
        QFETCH(QString, fault);
        Fixture fixture;
        FilesystemRunner runner;
        ProtonProvider provider(runner);
        QVERIFY(QDir().mkpath(runner.remoteFile("/my-files")));
        fixture.request.remoteSetRoot = "/my-files/" + fixture.request.remoteSetRoot;
        QVERIFY(fixture.source("a", "old"));
        QString error, confirmation;
        QJsonObject proposal;
        QVERIFY2(IncrementalRepository::initialize(fixture.request, provider, true, &error), qPrintable(error));
        QVERIFY(IncrementalRepository::execute(fixture.request, provider).success());
        fixture.next();
        QVERIFY(fixture.source("a", "new"));
        fixture.request.retention = 1;
        const auto result = IncrementalRepository::execute(fixture.request, provider);
        QVERIFY2(result.success(), qPrintable(errors(result)));
        QVERIFY2(!result.cleanupConfirmation.isEmpty(), qPrintable(errors(result)));
        fixture.request.cleanupConfirmation = result.cleanupConfirmation;
        if (fault == "trash" || fault == "delete") runner.loseMutationResponse = fault;
        else if (fault == "item failure") runner.rejectMutation = true;
        else runner.duplicateTrashName = true;
        QVERIFY(!IncrementalRepository::cleanup(fixture.request, provider, &proposal, &confirmation, &error));
        runner.loseMutationResponse.clear();
        runner.rejectMutation = false;
        runner.duplicateTrashName = false;
        QVERIFY2(IncrementalRepository::cleanup(fixture.request, provider, &proposal, &confirmation, &error), qPrintable(error));
        for (const auto &call : runner.calls) {
            QVERIFY(!call.contains("empty-trash"));
            if (call.value(1) == "delete") QVERIFY(call.last().startsWith("/trash/"));
        }
        fixture.next();
        const auto unchanged = IncrementalRepository::execute(fixture.request, provider);
        QVERIFY2(unchanged.success(), qPrintable(errors(unchanged)));
        QCOMPARE(unchanged.state, IncrementalWriteResult::State::Unchanged);
    }

    void cleanupPreservesSharedMembersAndSuccessfulHistory()
    {
        Fixture fixture;
        QString error, confirmation;
        QJsonObject proposal;
        QVERIFY(fixture.source("a", "alpha"));
        QVERIFY(fixture.source("b", "beta"));
        QVERIFY2(fixture.initialize(&error), qPrintable(error));
        QVERIFY(IncrementalRepository::execute(fixture.request, fixture.provider).success());
        const auto first = fixture.request;
        const QString shared = fixture.objectNames().first();
        fixture.next();
        fixture.request.scan.sources.removeFirst(); // b still keeps the entire shared archive live.
        fixture.request.retention = 1;
        auto result = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(result.success(), qPrintable(errors(result)));
        QVERIFY(result.cleanupNeedsAttention);
        QVERIFY(errors(result).contains("Backup saved; cleanup needs attention"));
        QCOMPARE(result.cleanupProposal.value("objects").toArray().size(), 0);
        QCOMPARE(result.cleanupProposal.value("estimated_live_member_bytes").toInteger(), qint64(4));
        QCOMPARE(result.cleanupProposal.value("estimated_obsolete_member_bytes").toInteger(), qint64(5));
        QCOMPARE(fixture.provider.destructiveCalls, 0);
        fixture.request.cleanupConfirmation = result.cleanupConfirmation;
        fixture.provider.allowDeletion = true;
        QVERIFY2(fixture.cleanup(&proposal, &confirmation, &error), qPrintable(error));
        QVERIFY(fixture.snapshot(first.snapshotId).isEmpty());
        QVERIFY(QFileInfo::exists(fixture.cloud("objects/" + shared)));
        fixture.next();
        const auto unchanged = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(unchanged.success(), qPrintable(errors(unchanged)));
        QCOMPARE(unchanged.state, IncrementalWriteResult::State::Unchanged);
        fixture.request.retention = 0;
        // Old completed operations may be replayed, but must never republish expired snapshots.
        const auto replayed = IncrementalRepository::execute(first, fixture.provider);
        QVERIFY2(replayed.success(), qPrintable(errors(replayed)));
        QVERIFY(fixture.snapshot(first.snapshotId).isEmpty());
        const QString newOwner = uuid();
        QVERIFY2(IncrementalRepository::takeover(fixture.request, fixture.provider.local, newOwner, true, &error), qPrintable(error));
        fixture.request.computerId = newOwner;
        ++fixture.request.ownershipEpoch;
        fixture.next();
        const auto handedOff = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(handedOff.success(), qPrintable(errors(handedOff)));
        QCOMPARE(handedOff.state, IncrementalWriteResult::State::Unchanged);
    }

    void cleanupInterruptions_data()
    {
        QTest::addColumn<QString>("phase");
        QTest::newRow("authorized") << QString("cleanup-authorized");
        for (const auto &kind : {QString("snapshot"), QString("legacy"), QString("object")})
            for (const auto &state : {QString("trashed"), QString("deleted")})
                for (const auto &prefix : {QString("cleanup-before-"), QString("cleanup-after-"), QString("cleanup-")}) {
                    const QString phase = prefix + kind + '-' + state;
                    QTest::newRow(qPrintable(phase)) << phase;
                }
        QTest::newRow("done") << QString("cleanup-done");
    }

    void cleanupInterruptions()
    {
        QFETCH(QString, phase);
        Fixture fixture;
        QString error, confirmation;
        QJsonObject proposal;
        QVERIFY(fixture.source("a", "old bytes"));
        QVERIFY2(fixture.initialize(&error), qPrintable(error));
        QVERIFY(IncrementalRepository::execute(fixture.request, fixture.provider).success());
        const QString old = fixture.request.snapshotId;
        const QString oldObject = fixture.objectNames().first();
        QVERIFY(fixture.legacy("older", 100));
        fixture.next();
        QVERIFY(fixture.source("a", "new bytes"));
        fixture.request.retention = 1;
        const auto saved = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(saved.success(), qPrintable(errors(saved)));
        QVERIFY(saved.cleanupNeedsAttention);
        const auto retained = fixture.snapshot(fixture.request.snapshotId);
        fixture.request.cleanupConfirmation = saved.cleanupConfirmation;
        fixture.provider.allowDeletion = true;
        bool interrupted = false;
        fixture.request.checkpoint = [&](const QString &current) {
            if (current == phase) { interrupted = true; return IncrementalControl::Pause; }
            return IncrementalControl::Continue;
        };
        QVERIFY(!fixture.cleanup(&proposal, &confirmation, &error));
        QVERIFY2(interrupted, qPrintable(error));
        QCOMPARE(fixture.snapshot(fixture.request.snapshotId), retained);
        // Lose all local checkpoints. Remote immutable intent alone fixes retry scope.
        QVERIFY(QDir(fixture.request.stateDirectory).removeRecursively());
        fixture.request.checkpoint = {};
        fixture.request.cleanupConfirmation.clear(); // Persisted authorization survives restart.
        QVERIFY2(fixture.cleanup(&proposal, &confirmation, &error), qPrintable(error));
        QVERIFY(fixture.snapshot(old).isEmpty());
        QVERIFY(!QFileInfo::exists(fixture.cloud("objects/" + oldObject)));
        QCOMPARE(fixture.snapshot(fixture.request.snapshotId), retained);
        QVERIFY(!QFileInfo::exists(fixture.directory.filePath("cloud/legacy/Documents/older")));
        fixture.next();
        const auto check = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(check.success(), qPrintable(errors(check)));
        QCOMPARE(check.state, IncrementalWriteResult::State::Unchanged);
    }

    void cleanupFailsClosedAndResumesWriter()
    {
        Fixture fixture;
        QString error, confirmation;
        QJsonObject proposal;
        QVERIFY(fixture.source("a", "alpha"));
        QVERIFY(fixture.initialize(&error));
        QVERIFY(IncrementalRepository::execute(fixture.request, fixture.provider).success());
        const auto saved = fixture.request;
        fixture.next();
        QVERIFY(fixture.source("a", "changed"));
        fixture.request.checkpoint = [](const QString &phase) {
            return phase == "uploaded" ? IncrementalControl::Pause : IncrementalControl::Continue;
        };
        QCOMPARE(IncrementalRepository::execute(fixture.request, fixture.provider).state, IncrementalWriteResult::State::Paused);
        auto paused = fixture.request;
        fixture.request = saved;
        fixture.request.retention = 1;
        QVERIFY(!fixture.cleanup(&proposal, &confirmation, &error));
        QVERIFY(error.contains("resumable"));
        QCOMPARE(fixture.provider.destructiveCalls, 0);
        fixture.request = paused;
        fixture.request.checkpoint = {};
        QVERIFY(IncrementalRepository::execute(fixture.request, fixture.provider).success());
        fixture.request.retention = 1;
        const QString retainedPath = fixture.cloud("snapshots/" + fixture.request.snapshotId + "/manifest.json");
        const auto retained = fixture.snapshot(fixture.request.snapshotId);
        QVERIFY(damage(retainedPath, "unreadable-json"));
        QVERIFY(!fixture.cleanup(&proposal, &confirmation, &error));
        QCOMPARE(fixture.provider.destructiveCalls, 0);
        QVERIFY(damage(retainedPath, QJsonDocument(retained).toJson()));
        QVERIFY(!fixture.cleanup(&proposal, &confirmation, &error));
        QVERIFY(!confirmation.isEmpty());
        fixture.request.cleanupConfirmation = QString(64, '0');
        QVERIFY(!fixture.cleanup(&proposal, &confirmation, &error));
        QCOMPARE(fixture.provider.destructiveCalls, 0);
        fixture.request.cleanupConfirmation = confirmation;
        fixture.provider.allowDeletion = true;
        fixture.provider.failDelete = true;
        QVERIFY(!fixture.cleanup(&proposal, &confirmation, &error));
        const int calls = fixture.provider.destructiveCalls;
        // An unresolved delete intent blocks even the original completed writer.
        const auto blocked = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY(!blocked.success());
        QCOMPARE(fixture.provider.destructiveCalls, calls);
        fixture.provider.failDelete = false;
        fixture.provider.loseDeleteResponse = true;
        QVERIFY(!fixture.cleanup(&proposal, &confirmation, &error));
        fixture.provider.loseDeleteResponse = false;
        QVERIFY2(fixture.cleanup(&proposal, &confirmation, &error), qPrintable(error));
        QCOMPARE(fixture.snapshot(fixture.request.snapshotId), retained);
    }

    void cleanupEmptyMixedStatesAndQuotaFailure()
    {
        Fixture fixture;
        QString error, confirmation;
        QJsonObject proposal;
        QVERIFY(fixture.source("a", "alpha"));
        QVERIFY(fixture.initialize(&error));
        QVERIFY(IncrementalRepository::execute(fixture.request, fixture.provider).success());
        const QString old = fixture.request.snapshotId;
        QVERIFY(fixture.legacy("complete", 50));
        QVERIFY(fixture.legacy("incomplete", 10, true));
        fixture.next();
        QVERIFY(fixture.source("a", "changed"));
        fixture.request.retention = 1;
        fixture.provider.failUpload = ".tar.gz";
        const auto waiting = IncrementalRepository::execute(fixture.request, fixture.provider);
        QCOMPARE(waiting.state, IncrementalWriteResult::State::Waiting);
        QCOMPARE(fixture.provider.destructiveCalls, 0);
        QVERIFY(!fixture.snapshot(old).isEmpty());
        fixture.provider.failUpload.clear();
        fixture.request.scan.sources.clear(); // A complete empty replacement is a saved state.
        const auto saved = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(saved.success(), qPrintable(errors(saved)));
        QVERIFY(saved.cleanupNeedsAttention);
        QCOMPARE(saved.cleanupProposal.value("legacy").toArray().size(), 1);
        QCOMPARE(saved.cleanupProposal.value("snapshots").toArray().size(), 1);
        fixture.request.cleanupConfirmation = saved.cleanupConfirmation;
        fixture.provider.allowDeletion = true;
        QVERIFY2(fixture.cleanup(&proposal, &confirmation, &error), qPrintable(error));
        QCOMPARE(fixture.objectNames().size(), 0);
        QVERIFY(fixture.snapshot(old).isEmpty());
        QVERIFY(QFileInfo::exists(fixture.directory.filePath("cloud/legacy/Documents/incomplete/manifest.json")));
        fixture.next();
        const auto unchanged = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(unchanged.success(), qPrintable(errors(unchanged)));
        QCOMPARE(unchanged.state, IncrementalWriteResult::State::Unchanged);
    }

    void cleanupProtectsFreshReferencesAndRejectsUnsafeScopes()
    {
        Fixture fixture;
        QString error, confirmation;
        QJsonObject proposal;
        QVERIFY(fixture.source("a", "old"));
        QVERIFY(fixture.initialize(&error));
        QVERIFY(IncrementalRepository::execute(fixture.request, fixture.provider).success());
        const auto original = fixture.snapshot(fixture.request.snapshotId);
        const QString oldObject = fixture.objectNames().first();
        fixture.next();
        QVERIFY(fixture.source("a", "new"));
        fixture.request.retention = 1;
        const auto result = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY(result.success());
        fixture.request.cleanupConfirmation = result.cleanupConfirmation;
        const auto request = fixture.request;
        fixture.request.legacyRoot = fixture.request.remoteSetRoot + "/snapshots";
        // A foreign owner or out-of-root set selection cannot authorize any mutation.
        fixture.request.remoteSetRoot = "other/sets/" + uuid();
        QVERIFY(!fixture.cleanup(&proposal, &confirmation, &error));
        QCOMPARE(fixture.provider.destructiveCalls, 0);
        fixture.request = request;
        auto retained = fixture.snapshot(fixture.request.snapshotId);
        retained["archives"] = original.value("archives");
        retained["entries"] = original.value("entries");
        QVERIFY(damage(fixture.cloud("snapshots/" + fixture.request.snapshotId + "/manifest.json"), QJsonDocument(retained).toJson()));
        fixture.provider.allowDeletion = true;
        QVERIFY(!fixture.cleanup(&proposal, &confirmation, &error));
        QVERIFY2(error.contains("still referenced"), qPrintable(error));
        QVERIFY(QFileInfo::exists(fixture.cloud("objects/" + oldObject)));
        for (const auto &call : fixture.provider.deletions) QVERIFY(!call.endsWith("objects/" + oldObject));
    }

    void cancelledUnverifiedUploadsCanBeCollectedOnlyAfterSavedReplacement()
    {
        Fixture fixture;
        QString error, confirmation;
        QJsonObject proposal;
        QVERIFY(fixture.source("a", "orphaned bytes"));
        QVERIFY(fixture.initialize(&error));
        fixture.request.checkpoint = [](const QString &phase) {
            return phase == "uploaded" ? IncrementalControl::Pause : IncrementalControl::Continue;
        };
        QCOMPARE(IncrementalRepository::execute(fixture.request, fixture.provider).state, IncrementalWriteResult::State::Paused);
        fixture.request.checkpoint = [](const QString &) { return IncrementalControl::Cancel; };
        QCOMPARE(IncrementalRepository::execute(fixture.request, fixture.provider).state, IncrementalWriteResult::State::Cancelled);
        fixture.request.retention = 1;
        QVERIFY(!fixture.cleanup(&proposal, &confirmation, &error));
        QCOMPARE(fixture.provider.destructiveCalls, 0);
        fixture.next();
        fixture.request.scan.sources.clear();
        const auto saved = IncrementalRepository::execute(fixture.request, fixture.provider);
        QVERIFY2(saved.success(), qPrintable(errors(saved)));
        QCOMPARE(saved.cleanupProposal.value("objects").toArray().size(), 1);
        fixture.request.cleanupConfirmation = saved.cleanupConfirmation;
        fixture.provider.allowDeletion = true;
        QVERIFY2(fixture.cleanup(&proposal, &confirmation, &error), qPrintable(error));
        QCOMPARE(fixture.objectNames().size(), 0);
    }

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

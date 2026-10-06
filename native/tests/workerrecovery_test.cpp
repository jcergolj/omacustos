#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QStorageInfo>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QDirIterator>

#include "../src/backupconfig.h"
#include "../src/backuprunstore.h"
#include "../src/backupmanifest.h"
#include "../src/backuprestorecontroller.h"
#include "../src/protonprovider.h"
#include "../src/qprocessrunner.h"

namespace {
bool writeFile(const QString &path, const QByteArray &bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QVector<QJsonObject> logEntries(const QString &path)
{
    QFile file(path);
    QVector<QJsonObject> entries;
    if (!file.open(QIODevice::ReadOnly)) return entries;
    while (!file.atEnd()) entries.append(QJsonDocument::fromJson(file.readLine()).object());
    return entries;
}

class RestoreStagingProvider final : public BackupProvider {
public:
    explicit RestoreStagingProvider(BackupProvider &provider) : provider(provider) {}
    BackupProvider &provider;
    QStringList workspaces;
    bool retainedWorkspace = false;
    bool upload(const QString &source, const QString &path, QString *error) override { return provider.upload(source, path, error); }
    bool ensureDirectory(const QString &path, QString *error) override { return provider.ensureDirectory(path, error); }
    bool trash(const QString &path, QString *error) override { return provider.trash(path, error); }
    bool permanentlyDelete(const QString &path, QString *error) override { return provider.permanentlyDelete(path, error); }
    bool inspect(const QString &path, RemoteFile *file, QString *error) override { return provider.inspect(path, file, error); }
    bool list(const QString &path, QVector<RemoteItem> *items, QString *error) override { return provider.list(path, items, error); }
    bool download(const QString &path, const QString &destination, QString *error) override
    {
        for (const QString &workspace : workspaces) retainedWorkspace |= QFileInfo::exists(workspace);
        workspaces.append(QFileInfo(destination).absolutePath());
        return provider.download(path, destination, error);
    }
    qint64 stagedBytes() const
    {
        qint64 bytes = 0;
        for (const QString &workspace : workspaces) {
            QDirIterator files(workspace, QDir::Files, QDirIterator::Subdirectories);
            while (files.hasNext()) { files.next(); bytes += files.fileInfo().size(); }
        }
        return bytes;
    }
};

struct WorkerFixture {
    QTemporaryDir directory {QDir::current().filePath("worker-continuation-XXXXXX")};
    BackupConfig config;
    QString configPath = directory.filePath("settings.json");
    BackupRunStore runs {directory.filePath("omacustos-backup-runs.json")};
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    QString error;

    bool prepare(qint64 budget = 8)
    {
        if (!directory.isValid()) return false;
        const QString cli = directory.filePath("proton-fixture");
        if (!QFile::copy(QStringLiteral(OMACUSTOS_CONTINUATION_CLI), cli)
            || !QFile::setPermissions(cli, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) return false;
        config.protonBinary = cli;
        config.sets = {{"documents", "Documents", "/backups", {directory.filePath("source")}, {}}};
        config.sets[0].stagingDirectory = directory.filePath(".stage");
        config.sets[0].stagingBudget = budget;
        environment.insert("FIXTURE_REMOTE", directory.filePath("remote"));
        environment.insert("FIXTURE_LOG", directory.filePath("transfers.jsonl"));
        environment.insert("FIXTURE_BATCH_LOG", directory.filePath("batches.jsonl"));
        environment.insert("FIXTURE_MARKER", directory.filePath("blocked"));
        return QDir().mkpath(directory.filePath("remote")) && QDir().mkpath(directory.filePath("source"))
            && save() && runs.queueManual({"documents"}, true, &error);
    }

    bool save() { return BackupConfigStore(configPath).save(config, &error); }
    void start(QProcess &worker)
    {
        worker.setProcessEnvironment(environment);
        worker.start(QStringLiteral(OMACUSTOS_WORKER_BINARY), {"--config", configPath});
    }
    bool run()
    {
        QProcess worker;
        start(worker);
        if (!worker.waitForFinished(20000) || worker.exitCode() != 0) { error = worker.readAllStandardError(); return false; }
        return runs.load(&error);
    }
    const BackupRunRecord &record() const { return *runs.find("documents"); }
    QString remoteCopy() const { return directory.filePath("remote") + record().remoteCopyPath; }
    bool emptyStaging() const { return QDir(config.sets[0].stagingDirectory).entryList({"attempt-*"}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(); }
    int transfers(const QString &name) const
    {
        int count = 0;
        for (const auto &item : logEntries(directory.filePath("transfers.jsonl")))
            if (QFileInfo(item.value("remote").toString()).fileName() == name) ++count;
        return count;
    }
};
}

class WorkerRecoveryTest : public QObject
{
    Q_OBJECT

private slots:
    void recoveryPreservesTheLogicalRun();
    void terminatedWorkerRecovers_data();
    void terminatedWorkerRecovers();
    void boundedBatches_data();
    void boundedBatches();
    void rescanReusesUnchangedAndReconcilesEdits_data();
    void rescanReusesUnchangedAndReconcilesEdits();
    void controlsStopTransfersAndSurviveScheduler_data();
    void controlsStopTransfersAndSurviveScheduler();
    void storageWaitingPreservesCheckpoint_data();
    void storageWaitingPreservesCheckpoint();
    void stagingRecoveryProtectsActiveAndUnrelatedData();
    void stagingRejectsUnsafeLocations_data();
    void stagingRejectsUnsafeLocations();
    void uncheckpointedUploadsAndRemoteDamageAreReconciled_data();
    void uncheckpointedUploadsAndRemoteDamageAreReconciled();
    void namespaceChangesRemainRestorable_data();
    void namespaceChangesRemainRestorable();
    void prerequisiteWaitingCanBeCancelledWithoutTouchingHistoricalCopies();
    void sourceFailuresFinalizeIncompleteAndProtectSuccessfulCopies_data();
    void sourceFailuresFinalizeIncompleteAndProtectSuccessfulCopies();
    void archiveSourceFailuresWithoutSurvivors();
    void singleArchiveRoundTrip_data();
    void singleArchiveRoundTrip();
    void singleArchiveRejectsDamage_data();
    void singleArchiveRejectsDamage();
    void singleArchivePreparationRace_data();
    void singleArchivePreparationRace();
    void singleArchiveStorageFailure_data();
    void singleArchiveStorageFailure();
    void singleArchiveVerificationFailure_data();
    void singleArchiveVerificationFailure();
    void boundedArchivesRoundTrip_data();
    void boundedArchivesRoundTrip();
    void boundedArchivesStorageWaiting_data();
    void boundedArchivesStorageWaiting();
};

void WorkerRecoveryTest::boundedArchivesRoundTrip_data()
{
    QTest::addColumn<QString>("workload");
    QTest::newRow("incompressible input exceeds capacity") << QString("budget");
    QTest::newRow("target boundary and related directories") << QString("target");
    QTest::newRow("many tiny directories") << QString("tiny");
    QTest::newRow("multiple roots and reserved names") << QString("roots");
    QTest::newRow("whole oversized file") << QString("oversized");
}

void WorkerRecoveryTest::boundedArchivesRoundTrip()
{
    QFETCH(QString, workload);
    const qint64 budget = 200000;
    WorkerFixture fixture;
    QVERIFY(fixture.prepare(budget));
    fixture.environment.insert("OMACUSTOS_INTERNAL_BOUNDED_ARCHIVES", "1");
    fixture.environment.insert("OMACUSTOS_INTERNAL_ARCHIVE_TARGET", workload == "budget" ? "1000000000" : "40000");
    fixture.environment.insert("LD_PRELOAD", QStringLiteral(OMACUSTOS_QUOTA_FIXTURE));
    fixture.environment.insert("FIXTURE_STAGING_PEAK_LOG", fixture.directory.filePath("peak"));
    fixture.environment.insert("FIXTURE_TOTAL_STAGING_CAPACITY", workload == "oversized" ? "610000" : QString::number(budget));
    fixture.environment.insert("FIXTURE_COMMAND_LOG", fixture.directory.filePath("commands.jsonl"));
    QHash<QString, QByteArray> originals;
    const auto add = [&](const QString &relative, const QByteArray &bytes) {
        const QString path = fixture.directory.filePath(relative);
        originals.insert(path, bytes);
        return writeFile(path, bytes);
    };
    quint32 random = 12345;
    const auto noise = [&](int size) {
        QByteArray bytes(size, '\0');
        for (char &byte : bytes) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; byte = char(random); }
        return bytes;
    };
    for (int i = 0; i < 60; ++i)
        QVERIFY(add(QString("source/dir-%1/file-%2").arg(i / 5, 2, 10, QChar('0')).arg(i, 3, 10, QChar('0')), noise(4000)));
    if (workload == "tiny") {
        for (int i = 0; i < 80; ++i) QVERIFY(add(QString("source/tiny/%1/.hidden").arg(i), QByteArray(10, char(i))));
    }
    if (workload == "target") {
        // Lexical source order interleaves these parent files with a child
        // directory; direct-directory scheduling must keep them together.
        QVERIFY(add("source/related/a", noise(6000)));
        QVERIFY(add("source/related/z", noise(6000)));
        QVERIFY(add("source/related/sub/file", noise(30000)));
    }
    if (workload == "roots") {
        fixture.config.sets[0].sourceDirectories += {fixture.directory.filePath("other/source"), fixture.directory.filePath("manifest.json")};
        QVERIFY(add("other/source/dir-00/file-000", "different root"));
        QVERIFY(add("other/source/.hidden", "hidden root"));
        QVERIFY(add("other/source/.omacustos-archives/file", "reserved directory"));
        QVERIFY(add("source/manifest.json", "reserved file"));
        QVERIFY(add("manifest.json", "individual file"));
        QVERIFY(fixture.save());
    }
    if (workload == "oversized") QVERIFY(add("source/z-oversized", noise(300000)));
    QVERIFY(add("source/empty", {}));
    QVERIFY2(fixture.run(), qPrintable(fixture.error));
    QVERIFY2(fixture.record().status == "success", qPrintable(fixture.record().lastError));
    QCOMPARE(fixture.record().result.verifiedFiles, originals.size());
    QVERIFY(fixture.emptyStaging());
    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    QVERIFY2(BackupManifest::load(QDir(fixture.remoteCopy()).filePath("manifest.json"), &entries, &info, &fixture.error), qPrintable(fixture.error));
    QCOMPARE(info.version, 3);
    QHash<QString, QVector<BackupEntry>> archives;
    QSet<QString> restorePaths;
    for (const auto &entry : entries) {
        QCOMPARE(entry.size, qint64(originals.value(entry.sourcePath).size()));
        QCOMPARE(entry.checksum, QCryptographicHash::hash(originals.value(entry.sourcePath), QCryptographicHash::Sha256));
        QVERIFY(!restorePaths.contains(entry.restorePath));
        restorePaths.insert(entry.restorePath);
        archives[entry.archive.id].append(entry);
    }
    QVERIFY(archives.size() > 1);
    QVERIFY(archives.size() < originals.size() / 2); // tiny directories are not remote objects
    if (workload == "target") {
        QString parentArchive;
        for (const auto &entry : entries) {
            if (!entry.sourcePath.endsWith("/related/a") && !entry.sourcePath.endsWith("/related/z")) continue;
            if (parentArchive.isEmpty()) parentArchive = entry.archive.id;
            QCOMPARE(entry.archive.id, parentArchive);
        }
    }
    for (const auto &members : archives) {
        qint64 bytes = 0;
        QSet<QString> roots;
        for (const auto &entry : members) {
            bytes += entry.size;
            for (const auto &root : fixture.config.sets[0].sourceDirectories)
                if (entry.sourcePath == root || entry.sourcePath.startsWith(root + '/')) roots.insert(root);
        }
        QCOMPARE(roots.size(), 1);
        if (bytes > 40000 && workload != "budget") {
            QCOMPARE(members.size(), 1);
            QCOMPARE(bytes, qint64(300000));
        }
        QCOMPARE(fixture.transfers(members.first().archive.id), 1);
    }
    // Measure retained snapshot + compressed output bytes, not indexed payload.
    QFile peakFile(fixture.directory.filePath("peak"));
    QVERIFY(peakFile.open(QIODevice::ReadOnly));
    qint64 peak = 0;
    while (!peakFile.atEnd()) peak = qMax(peak, peakFile.readLine().trimmed().toLongLong());
    QVERIFY(peak > 0);
    QVERIFY2(peak <= (workload == "oversized" ? 610000 : budget), qPrintable(QString::number(peak)));
    if (workload == "oversized") QVERIFY(peak > 600000); // output and immutable snapshot really coexisted

    const QByteArray oldRemote = qgetenv("FIXTURE_REMOTE"), oldLog = qgetenv("FIXTURE_COMMAND_LOG");
    qputenv("FIXTURE_REMOTE", fixture.directory.filePath("remote").toUtf8());
    qputenv("FIXTURE_COMMAND_LOG", fixture.directory.filePath("restore-commands.jsonl").toUtf8());
    const auto reset = qScopeGuard([&] { qputenv("FIXTURE_REMOTE", oldRemote); qputenv("FIXTURE_COMMAND_LOG", oldLog); });
    QProcessRunner runner(fixture.config.protonBinary);
    ProtonProvider provider(runner);
    RestoreStagingProvider stagingProvider(provider);
    BackupEngine engine;
    BackupRestoreController controller(engine, &stagingProvider);
    QSignalSpy completed(&controller, &BackupRestoreController::restoreCompleted);
    QSignalSpy failed(&controller, &BackupRestoreController::failed);
    controller.loadManifest(QDir(fixture.remoteCopy()).filePath("manifest.json"));
    for (int operation = 0; operation < 3; ++operation) {
        QVariantList selected;
        QSet<QString> requiredArchives;
        for (int i = 0; i < entries.size(); ++i) {
            if (operation == 0 && i % 11 != 0) continue; // cross-archive selected files
            if (operation == 1 && !entries.at(i).sourcePath.contains("/dir-00/")) continue; // a folder
            selected.append(i);
            requiredArchives.insert(entries.at(i).archive.id);
        }
        QVERIFY(!selected.isEmpty());
        qint64 downloadBytes = 0;
        for (const QString &id : requiredArchives) downloadBytes += archives.value(id).first().archive.size;
        const auto cost = controller.downloadCost(selected);
        QCOMPARE(cost.value("bytes").toLongLong(), downloadBytes);
        QCOMPARE(cost.value("archiveCount").toInt(), requiredArchives.size());
        const int before = logEntries(fixture.directory.filePath("restore-commands.jsonl")).size();
        const QString destination = fixture.directory.filePath(QString("restore-%1").arg(operation));
        controller.restoreSelected(selected, destination);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(completed.size(), operation + 1);
        QSet<int> indexes;
        for (const auto &index : selected) indexes.insert(index.toInt());
        for (int i = 0; i < entries.size(); ++i) {
            QFile restored(QDir(destination).filePath(entries.at(i).restorePath));
            QCOMPARE(restored.exists(), indexes.contains(i));
            if (indexes.contains(i)) {
                QVERIFY(restored.open(QIODevice::ReadOnly));
                QCOMPARE(restored.readAll(), originals.value(entries.at(i).sourcePath));
            }
        }
        QSet<QString> downloaded;
        const auto commands = logEntries(fixture.directory.filePath("restore-commands.jsonl"));
        int downloads = 0;
        for (int i = before; i < commands.size(); ++i) {
            if (commands.at(i).value("command") != "download") continue;
            ++downloads;
            const auto args = commands.at(i).value("args").toArray();
            downloaded.insert(QFileInfo(args.at(args.size() - 2).toString()).fileName());
        }
        QCOMPARE(downloaded, requiredArchives);
        QCOMPARE(downloads, requiredArchives.size());
        QVERIFY(!stagingProvider.retainedWorkspace);
        for (const QString &workspace : stagingProvider.workspaces) QVERIFY(!QFileInfo::exists(workspace));
    }

    // Sample synchronously at the engine's placement boundary: compressed body
    // plus only this archive's selected payloads must be the entire workspace.
    qint64 restorePeak = 0;
    qint64 bound = 0;
    for (const auto &members : archives) {
        qint64 bytes = members.first().archive.size;
        for (const auto &entry : members) bytes += entry.size;
        bound = qMax(bound, bytes);
    }
    const auto restored = engine.restoreFiles({entries, fixture.directory.filePath("measured-restore"), controller.currentCopyPath()},
        stagingProvider, [&](int) { restorePeak = qMax(restorePeak, stagingProvider.stagedBytes()); });
    QVERIFY2(restored.success, qPrintable(restored.error));
    QVERIFY(restorePeak > 0);
    QVERIFY(restorePeak <= bound);
    QVERIFY(!stagingProvider.retainedWorkspace);
    for (const QString &workspace : stagingProvider.workspaces) QVERIFY(!QFileInfo::exists(workspace));
}

void WorkerRecoveryTest::boundedArchivesStorageWaiting_data()
{
    QTest::addColumn<QString>("failure");
    QTest::newRow("oversized conservative preflight after verified work") << QString("oversized");
    QTest::newRow("compressed output quota") << QString("archive");
    QTest::newRow("immutable snapshot quota") << QString("snapshot");
    QTest::newRow("durable checkpoint quota") << QString("checkpoint");
    QTest::newRow("archive write EIO after verified work") << QString("write-eio");
    QTest::newRow("archive flush EIO after verified work") << QString("flush-eio");
}

void WorkerRecoveryTest::boundedArchivesStorageWaiting()
{
    QFETCH(QString, failure);
    WorkerFixture fixture;
    QVERIFY(fixture.prepare(200000));
    fixture.environment.insert("OMACUSTOS_INTERNAL_BOUNDED_ARCHIVES", "1");
    fixture.environment.insert("OMACUSTOS_INTERNAL_ARCHIVE_TARGET", "40000");
    fixture.environment.insert("LD_PRELOAD", QStringLiteral(OMACUSTOS_QUOTA_FIXTURE));
    if (failure == "oversized") fixture.environment.insert("FIXTURE_STAGING_AVAILABLE", QString::number(16 * 1024 * 1024 + 200000));
    if (failure == "archive") fixture.environment.insert("FIXTURE_ARCHIVE_QUOTA", "1");
    if (failure == "snapshot") fixture.environment.insert("FIXTURE_STAGING_QUOTA", "1");
    if (failure == "checkpoint") fixture.environment.insert("FIXTURE_CHECKPOINT_FAILURE", "yes");
    if (failure == "write-eio") fixture.environment.insert("FIXTURE_ARCHIVE_WRITE_EIO_AFTER", "1");
    if (failure == "flush-eio") fixture.environment.insert("FIXTURE_ARCHIVE_FLUSH_EIO_AFTER", "1");
    QVERIFY(writeFile(fixture.directory.filePath("source/a-small"), QByteArray(10000, 'a')));
    QVERIFY(writeFile(fixture.directory.filePath("source/z-large"), QByteArray(300000, 'b')));
    QVERIFY(fixture.run());
    QCOMPARE(fixture.record().status, QString("waiting"));
    QVERIFY(fixture.record().unfinished);
    QVERIFY(!fixture.record().result.manifestVerified);
    const bool retainedWork = failure == "oversized" || failure.endsWith("-eio");
    QCOMPARE(fixture.record().result.verifiedFiles, retainedWork ? 1 : 0);
    if (retainedWork) {
        QVERIFY(fixture.record().lastError.contains("choose another staging disk"));
        if (failure == "oversized") {
            QVERIFY(fixture.record().lastError.contains("oversized"));
            QVERIFY(fixture.record().lastError.contains("bytes"));
        }
        const QString checkpoint = fixture.directory.filePath("continuations/")
            + QString::fromLatin1(QCryptographicHash::hash(fixture.record().remoteCopyPath.toUtf8(), QCryptographicHash::Sha256).toHex());
        int verified = 0;
        for (const QString &name : QDir(checkpoint).entryList({"batch-*.json"}, QDir::Files)) {
            QFile journal(QDir(checkpoint).filePath(name));
            QVERIFY(journal.open(QIODevice::ReadOnly));
            const auto object = QJsonDocument::fromJson(journal.readAll()).object();
            if (!object.value("verified").toBool()) continue;
            ++verified;
            const auto entry = object.value("entries").toArray().first().toObject();
            QCOMPARE(object.value("archives").toArray().size(), 1);
            const auto archive = object.value("archives").toArray().first().toObject();
            QVERIFY(!archive.value("id").toString().isEmpty());
            QCOMPARE(entry.value("archive_id").toString(), archive.value("id").toString());
            QCOMPARE(archive.value("members").toArray().size(), 1);
            QCOMPARE(entry.value("member").toString(), archive.value("members").toArray().first().toString());
            QFile remote(fixture.directory.filePath("remote") + archive.value("remote").toString());
            QVERIFY(remote.open(QIODevice::ReadOnly));
            QCOMPARE(remote.size(), archive.value("size").toInteger());
            QCOMPARE(QCryptographicHash::hash(remote.readAll(), QCryptographicHash::Sha256).toHex(), archive.value("sha256").toString().toLatin1());
        }
        QCOMPARE(verified, 1);
    }
    QVERIFY(!QFileInfo::exists(QDir(fixture.remoteCopy()).filePath("manifest.json")));
    QVERIFY(fixture.emptyStaging());
}

void WorkerRecoveryTest::singleArchiveVerificationFailure_data()
{
    QTest::addColumn<QString>("failure");
    QTest::newRow("archive content metadata required") << QString("storage-size");
    QTest::newRow("archive checksum required") << QString("archive");
    QTest::newRow("index checksum required") << QString("index");
}

void WorkerRecoveryTest::singleArchiveVerificationFailure()
{
    QFETCH(QString, failure);
    WorkerFixture fixture;
    QVERIFY(fixture.prepare(1024 * 1024));
    fixture.environment.insert("OMACUSTOS_INTERNAL_SINGLE_ARCHIVE", "1");
    if (failure == "storage-size") fixture.environment.insert("FIXTURE_ARCHIVE_STORAGE_SIZE_ONLY", "yes");
    if (failure == "archive") fixture.environment.insert("FIXTURE_CORRUPT_ARCHIVE", "yes");
    if (failure == "index") fixture.environment.insert("FIXTURE_CORRUPT_ITEM", "manifest.json");
    QVERIFY(writeFile(fixture.directory.filePath("source/file"), "ordinary bytes"));
    QVERIFY(fixture.run());
    QCOMPARE(fixture.record().status, QString("retrying"));
    QVERIFY(fixture.record().unfinished);
    QVERIFY(!fixture.record().result.manifestVerified);
    QVERIFY(!fixture.record().lastSuccess.isValid());
    if (failure != "index") QCOMPARE(fixture.record().result.verifiedFiles, 0);
    QVERIFY(fixture.emptyStaging());
}

void WorkerRecoveryTest::singleArchivePreparationRace_data()
{
    QTest::addColumn<bool>("replace");
    QTest::newRow("contents change while descriptor is read") << false;
    QTest::newRow("pathname replacement while descriptor is read") << true;
}

void WorkerRecoveryTest::singleArchivePreparationRace()
{
    QFETCH(bool, replace);
    WorkerFixture fixture;
    QVERIFY(fixture.prepare(10 * 1024 * 1024));
    const QString source = fixture.directory.filePath("source/file");
    QVERIFY(writeFile(source, QByteArray(3 * 1024 * 1024, 'a')));
    fixture.environment.insert("OMACUSTOS_INTERNAL_SINGLE_ARCHIVE", "1");
    fixture.environment.insert("LD_PRELOAD", QStringLiteral(OMACUSTOS_QUOTA_FIXTURE));
    fixture.environment.insert("FIXTURE_MUTATE_DURING_STAGING", source);
    if (replace) fixture.environment.insert("FIXTURE_REPLACE_DURING_STAGING", "yes");
    QVERIFY2(fixture.run(), qPrintable(fixture.error));
    QCOMPARE(fixture.record().status, QString("success"));
    QVector<BackupEntry> entries;
    QVERIFY(BackupManifest::load(QDir(fixture.remoteCopy()).filePath("manifest.json"), &entries));
    QCOMPARE(entries.size(), 1);
    const QByteArray expected = replace ? QByteArray(3 * 1024 * 1024, 'a')
        : QByteArray(1024 * 1024, 'a') + QByteArray(2 * 1024 * 1024, 'b');
    QCOMPARE(entries.first().size, qint64(expected.size()));
    QCOMPARE(entries.first().checksum, QCryptographicHash::hash(expected, QCryptographicHash::Sha256));
    const QByteArray old = qgetenv("FIXTURE_REMOTE");
    qputenv("FIXTURE_REMOTE", fixture.directory.filePath("remote").toUtf8());
    const auto reset = qScopeGuard([&] { qputenv("FIXTURE_REMOTE", old); });
    QProcessRunner runner(fixture.config.protonBinary);
    ProtonProvider provider(runner);
    BackupEngine engine;
    BackupRestoreController controller(engine, &provider);
    QSignalSpy completed(&controller, &BackupRestoreController::restoreCompleted);
    controller.loadManifest(QDir(fixture.remoteCopy()).filePath("manifest.json"));
    controller.restoreSelected({0}, fixture.directory.filePath("restore"));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
    QCOMPARE(completed.size(), 1);
    QFile restored(fixture.directory.filePath("restore/file"));
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), expected);
}

void WorkerRecoveryTest::singleArchiveStorageFailure_data()
{
    QTest::addColumn<QString>("failure");
    QTest::newRow("single archive allowance") << QString("budget");
    QTest::newRow("snapshot quota") << QString("snapshot");
    QTest::newRow("archive quota") << QString("archive");
}

void WorkerRecoveryTest::singleArchiveStorageFailure()
{
    QFETCH(QString, failure);
    WorkerFixture fixture;
    QVERIFY(fixture.prepare(failure == "budget" ? 8 : 1024 * 1024));
    fixture.environment.insert("OMACUSTOS_INTERNAL_SINGLE_ARCHIVE", "1");
    if (failure != "budget") {
        fixture.environment.insert("LD_PRELOAD", QStringLiteral(OMACUSTOS_QUOTA_FIXTURE));
        fixture.environment.insert(failure == "snapshot" ? "FIXTURE_STAGING_QUOTA" : "FIXTURE_ARCHIVE_QUOTA", "1");
    }
    QVERIFY(writeFile(fixture.directory.filePath("source/file"), "ordinary bytes"));
    QVERIFY2(fixture.run(), qPrintable(fixture.error));
    QCOMPARE(fixture.record().status, QString("waiting"));
    QVERIFY(fixture.record().unfinished);
    QVERIFY(!fixture.record().result.manifestVerified);
    QCOMPARE(fixture.record().result.verifiedFiles, 0);
    QVERIFY(!QFileInfo::exists(QDir(fixture.remoteCopy()).filePath("manifest.json")));
    QVERIFY(fixture.emptyStaging());
}

void WorkerRecoveryTest::singleArchiveRoundTrip_data()
{
    QTest::addColumn<bool>("sizeOnly");
    QTest::newRow("sha256 metadata") << false;
    QTest::newRow("content-size metadata") << true;
}

void WorkerRecoveryTest::singleArchiveRoundTrip()
{
    QFETCH(bool, sizeOnly);
    WorkerFixture fixture;
    QVERIFY(fixture.prepare(4 * 1024 * 1024));
    fixture.environment.insert("OMACUSTOS_INTERNAL_SINGLE_ARCHIVE", "1");
    fixture.environment.insert("FIXTURE_COMMAND_LOG", fixture.directory.filePath("commands.jsonl"));
    if (sizeOnly) fixture.environment.insert("FIXTURE_SIZE_ONLY", "yes");
    fixture.config.sets[0].exclusions = {"ignored", "link"};
    QVERIFY(fixture.save());
    const QByteArray original("original archived bytes");
    const QString changed = fixture.directory.filePath("source/changing");
    fixture.environment.insert("FIXTURE_REPLACE_SOURCE", changed);
    QVERIFY(writeFile(changed, original));
    QVERIFY(writeFile(fixture.directory.filePath("source/.hidden"), "hidden"));
    QVERIFY(writeFile(fixture.directory.filePath("source/empty"), {}));
    QVERIFY(writeFile(fixture.directory.filePath("source/manifest.json"), "a source named like the index"));
    QVERIFY(writeFile(fixture.directory.filePath("source/ignored/no"), "excluded"));
    QVERIFY(QFile::link(changed, fixture.directory.filePath("source/link")));
    for (int i = 0; i < 80; ++i)
        QVERIFY(writeFile(fixture.directory.filePath(QString("source/nested/%1/file").arg(i)), QByteArray(100, char(i))));
    QVERIFY2(fixture.run(), qPrintable(fixture.error));
    QCOMPARE(fixture.record().status, QString("success"));
    QCOMPARE(fixture.record().result.verifiedFiles, 84);
    QCOMPARE(fixture.record().result.verifiedBytes,
        qint64(original.size() + 6 + QByteArray("a source named like the index").size() + 80 * 100));
    QVERIFY(fixture.record().result.manifestVerified);
    QVERIFY(fixture.emptyStaging());
    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    QVERIFY2(BackupManifest::load(QDir(fixture.remoteCopy()).filePath("manifest.json"), &entries, &info, &fixture.error), qPrintable(fixture.error));
    QCOMPARE(info.version, 3);
    QCOMPARE(info.status, QString("complete"));
    QCOMPARE(entries.size(), 84);
    const BackupArchive archive = entries.first().archive;
    QVERIFY(archive.size > 0);
    QCOMPARE(fixture.transfers(QFileInfo(archive.remotePath).fileName()), 1);
    QCOMPARE(logEntries(fixture.directory.filePath("transfers.jsonl")).size(), 2); // archive + index
    // Independently parse the actual remote tar.gz; test gzip/tar interoperability.
    QProcess python;
    python.start("python3", {"-c", "import sys,tarfile,gzip; p=sys.argv[1]; t=tarfile.open(p); assert len(t.getmembers())==84; assert all(m.isfile() for m in t.getmembers()); b=open(p,'rb').read(); assert b[10:-8]==gzip.compress(gzip.decompress(b),compresslevel=3,mtime=0)[10:-8]", fixture.directory.filePath("remote") + archive.remotePath});
    QVERIFY(python.waitForFinished());
    QCOMPARE(python.exitCode(), 0);
    for (const auto &command : logEntries(fixture.directory.filePath("commands.jsonl"))) {
        if (command.value("command") == "download") {
            const auto args = command.value("args").toArray();
            QCOMPARE(QFileInfo(args.at(args.size() - 2).toString()).fileName(), QString("manifest.json"));
        }
    } // retention discovery may read the index, never the fresh archive body

    // The controller uses the real subprocess provider against the worker's remote bytes.
    const QByteArray oldRemote = qgetenv("FIXTURE_REMOTE");
    const QByteArray oldLog = qgetenv("FIXTURE_COMMAND_LOG");
    const QByteArray oldSize = qgetenv("FIXTURE_SIZE_ONLY");
    qputenv("FIXTURE_REMOTE", fixture.directory.filePath("remote").toUtf8());
    qputenv("FIXTURE_COMMAND_LOG", fixture.directory.filePath("restore-commands.jsonl").toUtf8());
    qputenv("FIXTURE_SIZE_ONLY", sizeOnly ? "yes" : "no");
    const auto resetEnvironment = qScopeGuard([&] {
        qputenv("FIXTURE_REMOTE", oldRemote); qputenv("FIXTURE_COMMAND_LOG", oldLog); qputenv("FIXTURE_SIZE_ONLY", oldSize);
    });
    QProcessRunner runner(fixture.config.protonBinary);
    ProtonProvider provider(runner);
    BackupEngine engine;
    BackupRestoreController controller(engine, &provider);
    QSignalSpy completed(&controller, &BackupRestoreController::restoreCompleted);
    QSignalSpy failed(&controller, &BackupRestoreController::failed);
    controller.discover(QFileInfo(fixture.record().remoteCopyPath).path(), "documents");
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
    QCOMPARE(controller.copies().size(), 1);
    controller.selectCopy(0);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
    QCOMPARE(controller.entries().size(), 84);
    QVERIFY(controller.restoreEligible());
    int downloads = 0, metadata = 0;
    for (const auto &command : logEntries(fixture.directory.filePath("restore-commands.jsonl"))) {
        if (command.value("command") == "download") ++downloads;
        if (command.value("command") == "info" || command.value("command") == "list") ++metadata;
    }
    QCOMPARE(downloads, 1); // only the manifest to display the picker
    QVERIFY(metadata <= 3); // archive count, not 84 logical files
    const QStringList paths = controller.entries();
    QVariantList selection {paths.indexOf(changed), paths.indexOf(fixture.directory.filePath("source/.hidden")),
        paths.indexOf(fixture.directory.filePath("source/empty"))};
    for (const auto &index : selection) QVERIFY(index.toInt() >= 0);
    const QString destination = fixture.directory.filePath("restored");
    controller.restoreSelected(selection, destination);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
    QCOMPARE(failed.size(), 0);
    QCOMPARE(completed.size(), 1);
    QFile restored(QDir(destination).filePath("changing"));
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), original); // pathname was replaced at upload
    QVERIFY(QFileInfo::exists(QDir(destination).filePath(".hidden")));
    QCOMPARE(QFileInfo(QDir(destination).filePath("empty")).size(), qint64(0));
    QVERIFY(!QFileInfo::exists(QDir(destination).filePath("nested")));
    QVERIFY(!QFileInfo::exists(QDir(destination).filePath("manifest.json")));
    downloads = 0;
    for (const auto &command : logEntries(fixture.directory.filePath("restore-commands.jsonl")))
        if (command.value("command") == "download") ++downloads;
    QCOMPARE(downloads, 2); // one archive for three selected members
    if (!sizeOnly) {
        const QString archivedCopy = fixture.record().remoteCopyPath;
        fixture.environment.remove("OMACUSTOS_INTERNAL_SINGLE_ARCHIVE");
        fixture.environment.remove("FIXTURE_REPLACE_SOURCE");
        QVERIFY(fixture.runs.queueManual({"documents"}, true, &fixture.error));
        QVERIFY(fixture.run());
        QVERIFY2(fixture.record().status == "success", qPrintable(fixture.record().lastError));
        QVERIFY(fixture.record().remoteCopyPath != archivedCopy);
        QVector<BackupEntry> legacy;
        QVERIFY(BackupManifest::load(QDir(fixture.remoteCopy()).filePath("manifest.json"), &legacy, &info));
        QCOMPARE(info.version, 2);
        controller.discover(QFileInfo(fixture.record().remoteCopyPath).path(), "documents");
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(controller.copies().size(), 2);
        controller.selectCopy(0);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(controller.currentCopyPath(), fixture.record().remoteCopyPath);
        const QString legacyDestination = fixture.directory.filePath("legacy-restored");
        controller.restoreSelected({controller.entries().indexOf(changed)}, legacyDestination);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(completed.size(), 2);
        QFile legacyFile(QDir(legacyDestination).filePath("changing"));
        QVERIFY(legacyFile.open(QIODevice::ReadOnly));
        QCOMPARE(legacyFile.readAll(), QByteArray("changed pathname after preparation"));
        controller.selectCopy(1);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(controller.currentCopyPath(), archivedCopy);
        QCOMPARE(controller.entries().size(), 84);
        QVERIFY(controller.restoreEligible());
    }
}

void WorkerRecoveryTest::singleArchiveRejectsDamage_data()
{
    QTest::addColumn<QString>("damage");
    for (const QString &value : {"checksum", "archive-size", "member-checksum", "member-size", "truncated", "traversal", "absolute", "symlink", "hardlink", "duplicate", "missing-member", "bad-gzip", "bad-tar", "gzip-crc", "tar-end", "trailing-tar", "unindexed"})
        QTest::newRow(qPrintable(value)) << value;
}

void WorkerRecoveryTest::singleArchiveRejectsDamage()
{
    QFETCH(QString, damage);
    WorkerFixture fixture;
    QVERIFY(fixture.prepare(1024 * 1024));
    fixture.environment.insert("OMACUSTOS_INTERNAL_SINGLE_ARCHIVE", "1");
    QVERIFY(writeFile(fixture.directory.filePath("source/file"), "verified"));
    QVERIFY(fixture.run());
    QCOMPARE(fixture.record().status, QString("success"));
    const QString manifestPath = QDir(fixture.remoteCopy()).filePath("manifest.json");
    QFile manifest(manifestPath);
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    QJsonObject index = QJsonDocument::fromJson(manifest.readAll()).object();
    manifest.close();
    QJsonArray archives = index.value("archives").toArray();
    QJsonObject archive = archives.first().toObject();
    const QString archivePath = fixture.directory.filePath("remote") + archive.value("remote").toString();
    QJsonArray entries = index.value("entries").toArray();
    QJsonObject entry = entries.first().toObject();
    if (damage == "member-checksum" || damage == "member-size") {
        if (damage == "member-checksum") entry.insert("sha256", QString(64, '0'));
        else entry.insert("size", entry.value("size").toInteger() + 1);
        entries[0] = entry;
        index.insert("entries", entries);
    } else if (damage == "archive-size") {
        archive.insert("size", archive.value("size").toInteger() + 1);
        archives[0] = archive;
        index.insert("archives", archives);
    } else {
        QProcess python;
        const QString script =
            "import sys,tarfile,io,gzip\n"
            "p,k,m=sys.argv[1:]\n"
            "if k=='checksum':\n b=bytearray(open(p,'rb').read()); b[15]^=1; open(p,'wb').write(b)\n"
            "elif k=='truncated':\n b=open(p,'rb').read(); open(p,'wb').write(b[:len(b)//2])\n"
            "elif k=='bad-gzip':\n open(p,'wb').write(b'not a gzip stream')\n"
            "elif k=='bad-tar':\n open(p,'wb').write(gzip.compress(b'x'*512+b'\\0'*1024,compresslevel=3))\n"
            "elif k=='gzip-crc':\n b=bytearray(open(p,'rb').read()); b[-8]^=1; open(p,'wb').write(b)\n"
            "elif k=='tar-end':\n b=gzip.decompress(open(p,'rb').read()); open(p,'wb').write(gzip.compress(b[:1024],compresslevel=3))\n"
            "elif k=='trailing-tar':\n"
            " b=gzip.decompress(open(p,'rb').read()); extra=io.BytesIO()\n"
            " with tarfile.open(fileobj=extra,mode='w') as t:\n  h=tarfile.TarInfo('../escaped'); h.size=4; t.addfile(h,io.BytesIO(b'evil'))\n"
            " open(p,'wb').write(gzip.compress(b+extra.getvalue(),compresslevel=3))\n"
            "else:\n"
            " with tarfile.open(p,'w:gz',compresslevel=3) as t:\n"
            "  h=tarfile.TarInfo(m)\n"
            "  if k in ('symlink','hardlink'): h.type=tarfile.SYMTYPE if k=='symlink' else tarfile.LNKTYPE; h.linkname='../escaped'; t.addfile(h)\n"
            "  else: h.size=8; t.addfile(h,io.BytesIO(b'verified'))\n"
            "  if k not in ('missing-member','symlink','hardlink'):\n"
            "   n={'traversal':'../escaped','absolute':'/escaped','duplicate':m}.get(k,'link'); h=tarfile.TarInfo(n)\n"
            "   if k=='symlink': h.type=tarfile.SYMTYPE; h.linkname='../escaped'; t.addfile(h)\n"
            "   else: h.size=4; t.addfile(h,io.BytesIO(b'evil'))\n";
        const QString member = damage == "missing-member" ? "other" : entry.value("member").toString();
        python.start("python3", {"-c", script, archivePath, damage, member});
        QVERIFY(python.waitForFinished());
        QCOMPARE(python.exitCode(), 0);
        // Except checksum mismatch, certify compressed bytes to exercise the
        // parser rather than failing only the outer checksum gate.
        if (damage != "checksum") {
            QFile packed(archivePath);
            QVERIFY(packed.open(QIODevice::ReadOnly));
            const QByteArray bytes = packed.readAll();
            archive.insert("size", bytes.size());
            archive.insert("sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
            archives[0] = archive;
            index.insert("archives", archives);
        }
    }
    QVERIFY(writeFile(manifestPath, QJsonDocument(index).toJson()));
    const QByteArray old = qgetenv("FIXTURE_REMOTE");
    qputenv("FIXTURE_REMOTE", fixture.directory.filePath("remote").toUtf8());
    const auto reset = qScopeGuard([&] { qputenv("FIXTURE_REMOTE", old); });
    QProcessRunner runner(fixture.config.protonBinary);
    ProtonProvider provider(runner);
    RestoreStagingProvider stagingProvider(provider);
    BackupEngine engine;
    BackupRestoreController controller(engine, &stagingProvider);
    QSignalSpy completed(&controller, &BackupRestoreController::restoreCompleted);
    QSignalSpy failed(&controller, &BackupRestoreController::failed);
    // Local index loading is a public controller seam and allows outer-checksum
    // corruption to reach restore even when catalog metadata would reject it.
    controller.loadManifest(manifestPath);
    QCOMPARE(controller.entries().size(), 1);
    const QString destination = fixture.directory.filePath("restored");
    QVERIFY(writeFile(QDir(destination).filePath("file"), "existing destination"));
    controller.restoreSelected({0}, destination);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
    QCOMPARE(completed.size(), 0);
    QCOMPARE(failed.size(), 1);
    QFile existing(QDir(destination).filePath("file"));
    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), QByteArray("existing destination"));
    QVERIFY(!QFileInfo::exists(fixture.directory.filePath("escaped")));
    QVERIFY(!stagingProvider.workspaces.isEmpty());
    for (const QString &workspace : stagingProvider.workspaces) QVERIFY(!QFileInfo::exists(workspace));
}

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
    QCOMPARE(recovered.progress.totalFiles, 1);
    QVERIFY(recovered.progress.finalizing);
    QCOMPARE(recovered.progressElapsedMs, qint64(1000));
    QCOMPARE(recovered.progressUpdatedAt, scheduled);
    QCOMPARE(reopened.readyIndexes(now).size(), 2);
    QCOMPARE(reopened.find("pending")->status, QString("pending"));
    QCOMPARE(reopened.find("idle")->status, QString("idle"));
    QVERIFY(!reopened.enqueue("documents", "manual", now));
    QVERIFY(!reopened.recoverInterrupted(now));
}

void WorkerRecoveryTest::boundedBatches_data()
{
    QTest::addColumn<int>("count");
    QTest::addColumn<int>("bytes");
    QTest::addColumn<qint64>("budget");
    QTest::newRow("total exceeds capacity-sized budget") << 7 << 4 << qint64(8);
    QTest::newRow("file-count bound for tiny files") << 2005 << 0 << qint64(1000000);
    QTest::newRow("oversized files run alone") << 3 << 12 << qint64(8);
}

void WorkerRecoveryTest::boundedBatches()
{
    QFETCH(int, count);
    QFETCH(int, bytes);
    QFETCH(qint64, budget);
    WorkerFixture fixture;
    QVERIFY2(fixture.prepare(budget), qPrintable(fixture.error));
    if (count == 7) {
        // The entire selection exceeds this disk/quota-equivalent payload
        // capacity. Free-space hints and actual aggregate writes are independent.
        fixture.environment.insert("LD_PRELOAD", QStringLiteral(OMACUSTOS_QUOTA_FIXTURE));
        fixture.environment.insert("FIXTURE_STAGING_CAPACITY", QString::number(budget));
        fixture.environment.insert("FIXTURE_STAGING_AVAILABLE", QString::number(16 * 1024 * 1024 + budget));
    }
    for (int index = 0; index < count; ++index)
        QVERIFY(writeFile(fixture.directory.filePath(QStringLiteral("source/file-%1").arg(index, 4, 10, QChar('0'))), QByteArray(bytes, 'a')));
    QVERIFY2(fixture.run(), qPrintable(fixture.error));
    QCOMPARE(fixture.record().status, QString("success"));
    QCOMPARE(fixture.record().result.verifiedFiles, count);
    QCOMPARE(fixture.record().result.verifiedBytes, qint64(count) * bytes);
    QVERIFY(fixture.record().result.manifestVerified);
    QVERIFY(fixture.emptyStaging());
    const auto batches = logEntries(fixture.directory.filePath("batches.jsonl"));
    QVERIFY(batches.size() > 1);
    for (const auto &batch : batches) {
        const qint64 size = batch.value("bytes").toInteger();
        const int files = batch.value("files").toInt();
        QVERIFY(size <= budget || (files == 1 && size == bytes));
        QVERIFY(files <= 1000);
        QVERIFY(!batch.value("source").toString().startsWith("/tmp/"));
    }
    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    QVERIFY2(BackupManifest::load(QDir(fixture.remoteCopy()).filePath("manifest.json"), &entries, &info, &fixture.error), qPrintable(fixture.error));
    QCOMPARE(entries.size(), count);
    QCOMPARE(info.status, QString("complete"));
}

void WorkerRecoveryTest::rescanReusesUnchangedAndReconcilesEdits_data()
{
    QTest::addColumn<bool>("sizeOnly");
    QTest::newRow("checksum metadata") << false;
    QTest::newRow("size-only metadata needs content evidence") << true;
}

void WorkerRecoveryTest::rescanReusesUnchangedAndReconcilesEdits()
{
    QFETCH(bool, sizeOnly);
    WorkerFixture fixture;
    QVERIFY(fixture.prepare());
    fixture.environment.insert("FIXTURE_SIZE_ONLY", sizeOnly ? "yes" : "no");
    fixture.environment.insert("FIXTURE_BLOCK_ITEM", "c.txt");
    for (const QString &name : {"a.txt", "b.txt", "c.txt", "d.txt"})
        QVERIFY(writeFile(fixture.directory.filePath("source/" + name), "good"));
    QProcess worker;
    fixture.start(worker);
    QVERIFY(worker.waitForStarted());
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(fixture.directory.filePath("blocked")), 10000);
    QVERIFY(fixture.runs.load());
    const QString copy = fixture.record().remoteCopyPath;
    worker.kill();
    QVERIFY(worker.waitForFinished());
    QVERIFY(writeFile(fixture.directory.filePath("source/b.txt"), "edit"));
    QVERIFY(QFile::remove(fixture.directory.filePath("source/c.txt")));
    QVERIFY(writeFile(fixture.directory.filePath("source/e.txt"), "new!"));
    fixture.config.sets[0].exclusions = {fixture.directory.filePath("source/d.txt")};
    // Add a root with the same basename. Existing unprefixed mappings survive.
    const QString extra = fixture.directory.filePath("other/source");
    QVERIFY(writeFile(QDir(extra).filePath("a.txt"), "root"));
    fixture.config.sets[0].sourceDirectories.prepend(extra);
    QVERIFY(fixture.save());
    fixture.environment.remove("FIXTURE_BLOCK_ITEM");
    QVERIFY2(fixture.run(), qPrintable(fixture.error));
    QCOMPARE(fixture.record().remoteCopyPath, copy);
    QCOMPARE(fixture.record().status, QString("success"));
    QCOMPARE(fixture.transfers("a.txt"), 2); // original once, new root once
    QCOMPARE(fixture.transfers("b.txt"), 2); // same-size edit is never reused
    QCOMPARE(fixture.transfers("c.txt"), 1);
    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    QVERIFY(BackupManifest::load(QDir(fixture.remoteCopy()).filePath("manifest.json"), &entries, &info));
    QStringList restorePaths;
    for (const auto &entry : entries) restorePaths.append(entry.restorePath);
    restorePaths.sort();
    QCOMPARE(restorePaths, QStringList({"a.txt", "b.txt", "e.txt", "source/a.txt"}));
    QVERIFY(fixture.emptyStaging());
}

void WorkerRecoveryTest::controlsStopTransfersAndSurviveScheduler_data()
{
    QTest::addColumn<QString>("action");
    QTest::addColumn<bool>("crashBeforeAcknowledgement");
    QTest::newRow("pause") << QString("pause") << false;
    QTest::newRow("cancel") << QString("cancel") << false;
    QTest::newRow("pause survives worker crash") << QString("pause") << true;
    QTest::newRow("cancel survives worker crash") << QString("cancel") << true;
}

void WorkerRecoveryTest::controlsStopTransfersAndSurviveScheduler()
{
    QFETCH(QString, action);
    QFETCH(bool, crashBeforeAcknowledgement);
    WorkerFixture fixture;
    QVERIFY(fixture.prepare(4));
    QVERIFY(writeFile(fixture.directory.filePath("source/a.txt"), "good"));
    QVERIFY(writeFile(fixture.directory.filePath("source/b.txt"), "more"));
    fixture.environment.insert("FIXTURE_BLOCK_ITEM", "b.txt");
    QProcess worker;
    fixture.start(worker);
    QVERIFY(worker.waitForStarted());
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(fixture.directory.filePath("blocked")), 10000);
    QVERIFY(fixture.runs.load());
    const QString copy = fixture.record().remoteCopyPath;
    QVERIFY(fixture.runs.requestControl("documents", action));
    if (crashBeforeAcknowledgement) worker.kill();
    QVERIFY(worker.waitForFinished(5000));
    QVERIFY(fixture.runs.load());
    const QString status = action == "pause" ? "paused" : "cancelled";
    if (!crashBeforeAcknowledgement) {
        QCOMPARE(fixture.record().status, status);
        QVERIFY(fixture.emptyStaging());
    }
    fixture.environment.remove("FIXTURE_BLOCK_ITEM");
    QVERIFY(fixture.run()); // scheduler invocation respects durable control intent
    QCOMPARE(fixture.record().status, status);
    QCOMPARE(fixture.record().remoteCopyPath, copy);
    QVERIFY(fixture.emptyStaging());
    QVERIFY(!QFileInfo::exists(QDir(fixture.remoteCopy()).filePath("manifest.json")));
    if (action == "pause") {
        QVERIFY(fixture.runs.requestControl("documents", "resume"));
        QVERIFY(fixture.run());
        QCOMPARE(fixture.record().remoteCopyPath, copy);
        QCOMPARE(fixture.transfers("a.txt"), 1);
        QCOMPARE(fixture.transfers("b.txt"), 1); // upload-before-checkpoint reconciled
    } else {
        QVERIFY(!fixture.runs.requestControl("documents", "resume"));
        QVERIFY(fixture.runs.queueManual({"documents"}, true));
        QVERIFY(fixture.run());
        QVERIFY(fixture.record().remoteCopyPath != copy);
        QVERIFY(fixture.record().cancelledCopies.contains(copy));
    }
    QCOMPARE(fixture.record().status, QString("success"));
}

void WorkerRecoveryTest::storageWaitingPreservesCheckpoint_data()
{
    QTest::addColumn<QString>("failure");
    QTest::newRow("quota despite filesystem free space") << QString("quota");
    QTest::newRow("oversized file cannot fit") << QString("space");
}

void WorkerRecoveryTest::storageWaitingPreservesCheckpoint()
{
    QFETCH(QString, failure);
    WorkerFixture fixture;
    QVERIFY(fixture.prepare(4));
    QVERIFY(writeFile(fixture.directory.filePath("source/a.txt"), "good"));
    QVERIFY(writeFile(fixture.directory.filePath("source/z.txt"), "largepayload"));
    fixture.environment.insert("LD_PRELOAD", QStringLiteral(OMACUSTOS_QUOTA_FIXTURE));
    if (failure == "quota") fixture.environment.insert("FIXTURE_STAGING_QUOTA", "4");
    else fixture.environment.insert("FIXTURE_STAGING_AVAILABLE", QString::number(16 * 1024 * 1024 + 4));
    QVERIFY2(fixture.run(), qPrintable(fixture.error));
    QCOMPARE(fixture.record().status, QString("waiting"));
    QVERIFY(fixture.record().unfinished);
    QCOMPARE(fixture.record().result.verifiedFiles, 1);
    QCOMPARE(fixture.record().progress.verifiedFiles, 1);
    QVERIFY(!fixture.record().result.manifestVerified);
    QVERIFY(fixture.record().lastError.contains(failure == "quota" ? "quota" : "required", Qt::CaseInsensitive));
    const QString copy = fixture.record().remoteCopyPath;
    QVERIFY(fixture.emptyStaging());
    fixture.environment.remove("LD_PRELOAD");
    fixture.environment.remove("FIXTURE_STAGING_AVAILABLE");
    fixture.environment.remove("FIXTURE_STAGING_QUOTA");
    fixture.config.sets[0].stagingDirectory = fixture.directory.filePath("another-disk");
    QVERIFY(fixture.save());
    QVERIFY(fixture.runs.requestControl("documents", "resume"));
    QVERIFY2(fixture.run(), qPrintable(fixture.error));
    QCOMPARE(fixture.record().status, QString("success"));
    QCOMPARE(fixture.record().remoteCopyPath, copy);
    QCOMPARE(fixture.transfers("a.txt"), 1);
    QCOMPARE(fixture.record().result.verifiedFiles, 2);
}

void WorkerRecoveryTest::stagingRecoveryProtectsActiveAndUnrelatedData()
{
    WorkerFixture fixture;
    QVERIFY(fixture.prepare());
    QVERIFY(writeFile(fixture.directory.filePath("source/a.txt"), "good"));
    const QString base = fixture.config.sets[0].stagingDirectory;
    const QString orphan = QDir(base).filePath("attempt-orphan");
    const QString active = QDir(base).filePath("attempt-active");
    const QString unrelated = QDir(base).filePath("attempt-user-data");
    for (const auto &path : {orphan, active}) {
        QVERIFY(writeFile(QDir(path).filePath(".omacustos-owner"), "omacustos-staging-v1\n"));
        QVERIFY(writeFile(QDir(path).filePath("payloads/file"), "owned"));
    }
    QVERIFY(writeFile(QDir(unrelated).filePath("file"), "user"));
    QLockFile activeLock(active + ".lock");
    activeLock.setStaleLockTime(0);
    QVERIFY(activeLock.tryLock());
    QVERIFY(fixture.run());
    QCOMPARE(fixture.record().status, QString("success"));
    QVERIFY(!QFileInfo::exists(orphan));
    QVERIFY(QFileInfo::exists(QDir(active).filePath("payloads/file")));
    QVERIFY(QFileInfo::exists(QDir(unrelated).filePath("file")));
    QVERIFY(QFileInfo::exists(fixture.directory.filePath("source/a.txt")));
}

void WorkerRecoveryTest::stagingRejectsUnsafeLocations_data()
{
    QTest::addColumn<QString>("location");
    QTest::newRow("inside source") << QString("source/staging");
    QTest::newRow("source ancestor") << QString(".");
    QTest::newRow("RAM backed") << QString("/tmp/omacustos-invalid-test");
    QTest::newRow("symlink to source") << QString("alias/staging");
}

void WorkerRecoveryTest::stagingRejectsUnsafeLocations()
{
    QFETCH(QString, location);
    WorkerFixture fixture;
    QVERIFY(fixture.prepare());
    QVERIFY(writeFile(fixture.directory.filePath("source/a.txt"), "good"));
    if (location.startsWith("alias")) QVERIFY(QFile::link(fixture.directory.filePath("source"), fixture.directory.filePath("alias")));
    fixture.config.sets[0].stagingDirectory = location.startsWith('/') ? location : fixture.directory.filePath(location);
    QVERIFY(fixture.save());
    QVERIFY(fixture.run());
    QCOMPARE(fixture.record().status, QString("waiting"));
    QCOMPARE(fixture.record().result.verifiedFiles, 0);
    QVERIFY(fixture.record().lastError.contains(location.startsWith("/tmp") ? "disk-backed" : "overlap"));
    QVERIFY(logEntries(fixture.directory.filePath("transfers.jsonl")).isEmpty());
    QVERIFY(QFileInfo::exists(fixture.directory.filePath("source/a.txt")));
}

void WorkerRecoveryTest::uncheckpointedUploadsAndRemoteDamageAreReconciled_data()
{
    QTest::addColumn<QString>("failure");
    QTest::newRow("checkpoint publication failed after upload") << QString("checkpoint");
    QTest::newRow("partial merge failure") << QString("partial");
    QTest::newRow("missing previously verified remote payload") << QString("missing");
    QTest::newRow("same-sized corrupt previously verified remote payload") << QString("corrupt");
    QTest::newRow("manifest failure preserves batches") << QString("manifest");
}

void WorkerRecoveryTest::uncheckpointedUploadsAndRemoteDamageAreReconciled()
{
    QFETCH(QString, failure);
    WorkerFixture fixture;
    QVERIFY(fixture.prepare(8));
    fixture.environment.insert("FIXTURE_SIZE_ONLY", "yes");
    QVERIFY(writeFile(fixture.directory.filePath("source/a.txt"), "good"));
    QVERIFY(writeFile(fixture.directory.filePath("source/z.txt"), "more"));
    if (failure == "checkpoint") {
        fixture.environment.insert("LD_PRELOAD", QStringLiteral(OMACUSTOS_QUOTA_FIXTURE));
        fixture.environment.insert("FIXTURE_CHECKPOINT_FAILURE", "yes");
    } else fixture.environment.insert("FIXTURE_FAIL_ITEM", failure == "manifest" ? "manifest.json" : "z.txt");
    QVERIFY2(fixture.run(), qPrintable(fixture.error));
    QCOMPARE(fixture.record().status, failure == "checkpoint" ? QString("waiting") : QString("retrying"));
    QVERIFY(fixture.record().unfinished);
    QVERIFY(!fixture.record().result.manifestVerified);
    const QString copy = fixture.record().remoteCopyPath;
    const QString remoteA = QDir(fixture.remoteCopy()).filePath("a.txt");
    if (failure == "missing") QVERIFY(QFile::remove(remoteA));
    if (failure == "corrupt") QVERIFY(writeFile(remoteA, "evil"));
    fixture.environment.remove("LD_PRELOAD");
    fixture.environment.remove("FIXTURE_CHECKPOINT_FAILURE");
    fixture.environment.remove("FIXTURE_FAIL_ITEM");
    QVERIFY(fixture.runs.requestControl("documents", "resume"));
    QVERIFY2(fixture.run(), qPrintable(fixture.error));
    QCOMPARE(fixture.record().status, QString("success"));
    QCOMPARE(fixture.record().remoteCopyPath, copy);
    const bool damage = failure == "missing" || failure == "corrupt";
    QCOMPARE(fixture.transfers("a.txt"), damage ? 3 : failure == "partial" ? 2 : 1);
    // A partial folder command retries the same snapshot twice; already uploaded
    // matching remote bytes are reconciled on continuation, not uploaded again.
    QCOMPARE(fixture.transfers("z.txt"), 1);
    QFile restored(remoteA);
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), QByteArray("good"));
    QCOMPARE(fixture.record().result.verifiedFiles, 2);
    QVERIFY(fixture.emptyStaging());
}

void WorkerRecoveryTest::namespaceChangesRemainRestorable_data()
{
    QTest::addColumn<QString>("change");
    QTest::newRow("reserved manifest directory") << QString("manifest-directory");
    QTest::newRow("checkpointed file becomes directory") << QString("file-directory");
    QTest::newRow("new root overlaps original unprefixed subtree") << QString("new-root");
}

void WorkerRecoveryTest::namespaceChangesRemainRestorable()
{
    QFETCH(QString, change);
    WorkerFixture fixture;
    QVERIFY(fixture.prepare(4));
    QVERIFY(writeFile(fixture.directory.filePath("source/a.txt"), "good"));
    if (change == "manifest-directory") {
        QVERIFY(writeFile(fixture.directory.filePath("source/manifest.json/data.txt"), "data"));
        QVERIFY(writeFile(fixture.directory.filePath("source/manifest.json.1/extra.txt"), "more"));
    } else fixture.environment.insert("FIXTURE_FAIL_ITEM", "manifest.json");
    QVERIFY(fixture.run());
    const QString copy = fixture.record().remoteCopyPath;
    if (change == "file-directory") {
        QCOMPARE(fixture.record().status, QString("retrying"));
        QVERIFY(QFile::remove(fixture.directory.filePath("source/a.txt")));
        QVERIFY(writeFile(fixture.directory.filePath("source/a.txt/child.txt"), "child"));
    } else if (change == "new-root") {
        QCOMPARE(fixture.record().status, QString("retrying"));
        QVERIFY(writeFile(fixture.directory.filePath("source/source/a.txt"), "local"));
        const QString extra = fixture.directory.filePath("other/source");
        QVERIFY(writeFile(QDir(extra).filePath("a.txt"), "extra"));
        fixture.config.sets[0].sourceDirectories.prepend(extra);
        QVERIFY(fixture.save());
    }
    if (change != "manifest-directory") {
        fixture.environment.remove("FIXTURE_FAIL_ITEM");
        QVERIFY(fixture.runs.requestControl("documents", "resume"));
        QVERIFY2(fixture.run(), qPrintable(fixture.error));
    }
    QCOMPARE(fixture.record().status, QString("success"));
    QCOMPARE(fixture.record().remoteCopyPath, copy);
    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    QVERIFY(BackupManifest::load(QDir(fixture.remoteCopy()).filePath("manifest.json"), &entries, &info));
    QCOMPARE(entries.size(), change == "file-directory" ? 1 : 3);
    QSet<QString> restores;
    for (const auto &entry : entries) {
        QVERIFY(!restores.contains(entry.restorePath));
        restores.insert(entry.restorePath);
        QFile remote(fixture.directory.filePath("remote") + entry.remotePath);
        QVERIFY(remote.open(QIODevice::ReadOnly));
        QFile source(entry.sourcePath);
        QVERIFY(source.open(QIODevice::ReadOnly));
        QCOMPARE(remote.readAll(), source.readAll());
    }
    QCOMPARE(info.status, QString("complete"));
}

void WorkerRecoveryTest::prerequisiteWaitingCanBeCancelledWithoutTouchingHistoricalCopies()
{
    WorkerFixture fixture;
    QVERIFY(fixture.prepare());
    QVERIFY(fixture.runs.load());
    auto *record = fixture.runs.find("documents");
    record->remoteCopyPath = "/historical-successful-copy";
    record->lastSuccess = QDateTime::currentDateTime();
    QVERIFY(fixture.runs.save());
    QVERIFY(fixture.runs.waitForPrerequisite("documents", "Waiting for AC power", QDateTime::currentDateTime()));
    QVERIFY(fixture.runs.requestControl("documents", "cancel"));
    QCOMPARE(fixture.record().status, QString("cancelled"));
    QVERIFY(fixture.record().cancelledCopies.isEmpty());
    QCOMPARE(fixture.record().remoteCopyPath, QString("/historical-successful-copy"));
    QVERIFY(fixture.runs.readyIndexes(QDateTime::currentDateTime().addSecs(120)).isEmpty());
}

void WorkerRecoveryTest::sourceFailuresFinalizeIncompleteAndProtectSuccessfulCopies_data()
{
    QTest::addColumn<bool>("historyUsesSingleArchive");
    QTest::addColumn<bool>("currentRunUsesBoundedArchives");
    QTest::newRow("individual payloads") << false << false;
    QTest::newRow("single archive") << true << false;
    QTest::newRow("bounded archives with legacy history") << false << true;
    QTest::newRow("bounded archives with archive history") << true << true;
}

void WorkerRecoveryTest::sourceFailuresFinalizeIncompleteAndProtectSuccessfulCopies()
{
    QFETCH(bool, historyUsesSingleArchive);
    QFETCH(bool, currentRunUsesBoundedArchives);
    WorkerFixture fixture;
    QVERIFY(fixture.prepare(historyUsesSingleArchive || currentRunUsesBoundedArchives ? 1024 * 1024 : 4));
    if (historyUsesSingleArchive) fixture.environment.insert("OMACUSTOS_INTERNAL_SINGLE_ARCHIVE", "1");
    const QString missing = fixture.directory.filePath("source/a.txt");
    QVERIFY(writeFile(missing, "good"));
    QVERIFY(writeFile(fixture.directory.filePath("source/b.txt"), "more"));
    fixture.config.sets[0].retention = 1;
    QVERIFY(fixture.save());
    QVERIFY(fixture.run());
    QCOMPARE(fixture.record().status, QString("success"));
    const QString successfulCopy = fixture.remoteCopy();
    if (currentRunUsesBoundedArchives) {
        fixture.environment.remove("OMACUSTOS_INTERNAL_SINGLE_ARCHIVE");
        fixture.environment.insert("OMACUSTOS_INTERNAL_BOUNDED_ARCHIVES", "1");
        fixture.environment.insert("OMACUSTOS_INTERNAL_ARCHIVE_TARGET", "4");
    }
    fixture.environment.insert("FIXTURE_DELETE_SOURCE", missing);
    QVERIFY(fixture.runs.queueManual({"documents"}, true));
    QVERIFY(fixture.run());
    QCOMPARE(fixture.record().status, QString("incomplete"));
    QVERIFY(fixture.record().result.manifestVerified);
    QCOMPARE(fixture.record().result.verifiedFiles, 1);
    QCOMPARE(fixture.record().result.issues.size(), 1);
    QCOMPARE(fixture.record().result.issues.first().path, missing);
    QCOMPARE(fixture.record().result.issues.first().phase, QString("reading"));
    QVERIFY(QFileInfo::exists(QDir(successfulCopy).filePath("manifest.json")));
    if (!historyUsesSingleArchive) QVERIFY(QFileInfo::exists(QDir(successfulCopy).filePath("a.txt")));
    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    QVERIFY(BackupManifest::load(QDir(fixture.remoteCopy()).filePath("manifest.json"), &entries, &info));
    QCOMPARE(info.status, QString("incomplete"));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().restorePath, QString("b.txt"));
    if (historyUsesSingleArchive || currentRunUsesBoundedArchives) {
        QCOMPARE(info.version, 3);
        const QByteArray old = qgetenv("FIXTURE_REMOTE");
        qputenv("FIXTURE_REMOTE", fixture.directory.filePath("remote").toUtf8());
        const auto reset = qScopeGuard([&] { qputenv("FIXTURE_REMOTE", old); });
        QProcessRunner runner(fixture.config.protonBinary);
        ProtonProvider provider(runner);
        BackupEngine engine;
        BackupRestoreController controller(engine, &provider);
        QSignalSpy completed(&controller, &BackupRestoreController::restoreCompleted);
        controller.loadManifest(QDir(fixture.remoteCopy()).filePath("manifest.json"));
        controller.restoreSelected({0}, fixture.directory.filePath("recovered-incomplete"));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(completed.size(), 1);
        QFile recovered(fixture.directory.filePath("recovered-incomplete/b.txt"));
        QVERIFY(recovered.open(QIODevice::ReadOnly));
        QCOMPARE(recovered.readAll(), QByteArray("more"));
        QVERIFY(!QFileInfo::exists(fixture.directory.filePath("recovered-incomplete/a.txt")));
    }
}

void WorkerRecoveryTest::archiveSourceFailuresWithoutSurvivors()
{
    WorkerFixture fixture;
    QVERIFY(fixture.prepare(1024 * 1024));
    fixture.environment.insert("OMACUSTOS_INTERNAL_BOUNDED_ARCHIVES", "1");
    const QString source = fixture.directory.filePath("source/a.txt");
    QVERIFY(writeFile(source, "good"));
    fixture.config.sets[0].retention = 1;
    QVERIFY(fixture.save());
    QVERIFY(fixture.run());
    QCOMPARE(fixture.record().status, QString("success"));
    const QString successfulCopy = fixture.remoteCopy();
    fixture.environment.insert("FIXTURE_DELETE_SOURCE", source);
    QVERIFY(fixture.runs.queueManual({"documents"}, true));
    QVERIFY(fixture.run());
    QCOMPARE(fixture.record().status, QString("failed"));
    QVERIFY(!fixture.record().unfinished);
    QCOMPARE(fixture.record().result.verifiedFiles, 0);
    QCOMPARE(fixture.record().result.issues.size(), 1);
    QCOMPARE(fixture.record().result.issues.first().path, source);
    QCOMPARE(fixture.record().result.issues.first().phase, QString("reading"));
    QVERIFY(!fixture.record().result.issues.first().reason.isEmpty());
    QVERIFY(QFileInfo::exists(QDir(successfulCopy).filePath("manifest.json")));
    QVERIFY(fixture.emptyStaging());
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
    QCOMPARE(record.remoteCopyPath, interruptedCopy);
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

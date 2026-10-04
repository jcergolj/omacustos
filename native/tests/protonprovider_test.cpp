#include <QTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTemporaryDir>

#include "../src/backupengine.h"
#include "../src/backupmanifest.h"
#include "../src/backupcatalog.h"
#include "../src/backupecleanup.h"
#include "../src/protonprovider.h"
#include "protonclifixture.h"

class FakeRunner final : public ProcessRunner
{
public:
    QStringList arguments;
    ProcessOutput response;

    ProcessOutput run(const QStringList &requestedArguments) override
    {
        arguments = requestedArguments;
        return response;
    }
};

class ProtonProviderTest final : public QObject
{
    Q_OBJECT

private slots:
    void uploadUsesJsonCliArguments();
    void folderUploadPreservesSelectionSnapshotsAndCleansStaging_data();
    void folderUploadPreservesSelectionSnapshotsAndCleansStaging();
    void folderUploadFailuresCleanStagingAndKeepVerifiedFiles_data();
    void folderUploadFailuresCleanStagingAndKeepVerifiedFiles();
    void rejectsInvalidFolderUploadPaths_data();
    void rejectsInvalidFolderUploadPaths();
    void uploadsAtExactRequestedPath_data();
    void uploadsAtExactRequestedPath();
    void rejectsInvalidUploadPaths_data();
    void rejectsInvalidUploadPaths();
    void stagingFailureDoesNotUpload();
    void uploadFailurePreservesFilesAndCleansStaging();
    void backsUpAndRestoresReservedAndCollisionNames();
    void failedPayloadsAreNotVerified_data();
    void failedPayloadsAreNotVerified();
    void sharedDestinationDoesNotRepeatDirectoryChecks_data();
    void sharedDestinationDoesNotRepeatDirectoryChecks();
    void freshCopyVerificationRejectsChangedPayloads_data();
    void freshCopyVerificationRejectsChangedPayloads();
    void nestedDirectoriesShareAncestorsOnlyWithinOperation_data();
    void nestedDirectoriesShareAncestorsOnlyWithinOperation();
    void failedUploadRechecksMissingAncestors();
    void verifiesCopiesUsingBulkMetadataWithIndividualFallback_data();
    void verifiesCopiesUsingBulkMetadataWithIndividualFallback();
    void bulkMetadataIgnoresUnsafeAndUnverifiableEntries();
    void bulkVerificationPreservesRetentionAndCancellationGates_data();
    void bulkVerificationPreservesRetentionAndCancellationGates();
    void inspectParsesVerifiedMetadata();
    void inspectParsesCliMetadataWithoutSha256();
    void inspectUsesContentSizeInsteadOfEncryptedStorageSize();
    void inspectRejectsStorageSizeWithoutContentSize();
    void successfulInspectClearsEarlierErrors();
    void commandErrorsAreActionable();
    void rejectsNullMetadataOutput();
    void listsRemoteItemsAndUsesExactCleanupCommands();
};

void ProtonProviderTest::folderUploadPreservesSelectionSnapshotsAndCleansStaging_data()
{
    QTest::addColumn<bool>("multipleSources");
    QTest::newRow("one folder") << false;
    QTest::newRow("multiple folders and a file") << true;
}

void ProtonProviderTest::folderUploadPreservesSelectionSnapshotsAndCleansStaging()
{
    QFETCH(bool, multipleSources);
    QTemporaryDir source, destination;
    FilesystemRunner runner;
    runner.includeListingContentSize = true;
    const QString first = source.filePath("first/Documents");
    const QString second = source.filePath("second/Documents");
    const QStringList relativePaths {"notes.txt", "nested folder/.hidden", ".config/settings",
        "manifest.json", "manifest.json.1", "empty"};
    QHash<QString, QByteArray> expected;
    for (const QString &relative : relativePaths) {
        const QString path = QDir(first).filePath(relative);
        QVERIFY(QDir().mkpath(QFileInfo(path).path()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray contents = relative == "empty" ? QByteArray() : ("original " + relative).toUtf8();
        QCOMPARE(file.write(contents), contents.size());
        expected.insert(path, contents);
    }
    const QString excluded = QDir(first).filePath("cache/drop.txt");
    QVERIFY(QDir().mkpath(QFileInfo(excluded).path()));
    QFile excludedFile(excluded);
    QVERIFY(excludedFile.open(QIODevice::WriteOnly));
    excludedFile.write("excluded data");
    excludedFile.close();
    QVERIFY(QFile::link(QDir(first).filePath("notes.txt"), QDir(first).filePath("excluded-link")));
    QStringList sources {first};
    if (multipleSources) {
        QVERIFY(QDir().mkpath(second));
        const QString secondPath = QDir(second).filePath("notes.txt");
        const QString singlePath = source.filePath("single.txt");
        for (const QString &path : {secondPath, singlePath}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("second source"), qint64(13));
            expected.insert(path, "second source");
        }
        sources.append(second);
        sources.append(singlePath);
    }

    BackupEngine engine;
    ProtonProvider provider(runner);
    QString manifest, error;
    bool changedSource = false;
    bool sawVerification = false;
    const QString editedPath = QDir(first).filePath("notes.txt");
    const BackupCopyMetadata metadata {"computer", "documents", "Documents", "copy", QDateTime::currentDateTimeUtc()};
    QVERIFY2(engine.backup(sources, "/backups/copy", {"cache", "excluded-link"}, metadata,
        provider, &manifest, &error, [&](const BackupProgress &progress) {
            if (progress.phase == "staging") {
                QCOMPARE(progress.processedFiles, 0);
                QVERIFY(!progress.finalizing);
            }
            if (progress.phase == "uploading-folder") {
                QCOMPARE(progress.processedFiles, 0);
                QCOMPARE(progress.verifiedFiles, 0);
                QVERIFY(!progress.finalizing);
                // The single transfer must use immutable copies, even if a source
                // pathname is replaced after every snapshot has been prepared.
                QSaveFile edited(editedPath);
                QVERIFY(edited.open(QIODevice::WriteOnly));
                edited.write("changed after staging");
                QVERIFY(edited.commit());
                changedSource = true;
            }
            if (progress.phase == "verifying") {
                sawVerification = true;
                QCOMPARE(progress.processedFiles, expected.size());
                QVERIFY(!runner.uploadedPaths.isEmpty());
                QVERIFY(!QFileInfo::exists(QFileInfo(runner.uploadedPaths.first()).path()));
            }
        }, nullptr, {true}), qPrintable(error));
    const auto cleanup = qScopeGuard([&] { QDir(QFileInfo(manifest).path()).removeRecursively(); });
    QVERIFY(changedSource && sawVerification);
    QVector<QStringList> uploads;
    for (const auto &call : runner.calls) if (call.at(1) == "upload") uploads.append(call);
    QCOMPARE(uploads.size(), 2); // One recursive payload upload, then the manifest.
    QCOMPARE(uploads.first().at(6), QString("merge"));
    QCOMPARE(uploads.first().last(), QString("/backups"));
    QCOMPARE(QFileInfo(uploads.first().at(uploads.first().size() - 2)).fileName(), QString("copy"));
    QVERIFY(!QFileInfo::exists(QFileInfo(runner.uploadedPaths.first()).path()));
    QVERIFY(!QFileInfo::exists(runner.remoteFile("/backups/copy/")
        + (multipleSources ? "Documents/" : "") + "cache"));

    QVector<BackupEntry> entries;
    QVERIFY2(BackupManifest::load(manifest, &entries, &error), qPrintable(error));
    QCOMPARE(entries.size(), expected.size());
    QSet<QString> remotePaths;
    for (const BackupEntry &entry : entries) {
        QVERIFY(expected.contains(entry.sourcePath));
        QVERIFY(!remotePaths.contains(entry.remotePath));
        remotePaths.insert(entry.remotePath);
        QCOMPARE(entry.checksum, QCryptographicHash::hash(expected.value(entry.sourcePath), QCryptographicHash::Sha256));
        QVERIFY2(engine.restoreFile(entry, destination.path(), provider, &error), qPrintable(error));
        QFile restored(destination.filePath(entry.restorePath));
        QVERIFY(restored.open(QIODevice::ReadOnly));
        QCOMPARE(restored.readAll(), expected.value(entry.sourcePath));
    }
    QFile edited(editedPath);
    QVERIFY(edited.open(QIODevice::ReadOnly));
    QCOMPARE(edited.readAll(), QByteArray("changed after staging"));
}

void ProtonProviderTest::folderUploadFailuresCleanStagingAndKeepVerifiedFiles_data()
{
    QTest::addColumn<QString>("failure");
    QTest::newRow("entire folder upload fails") << QString("folder");
    QTest::newRow("partial folder upload fails") << QString("partial");
    QTest::newRow("remote file is truncated") << QString("verification");
    QTest::newRow("manifest upload fails") << QString("manifest");
    QTest::newRow("source disappears while staging") << QString("reading");
    QTest::newRow("source read fails after snapshot creation") << QString("read-error");
}

void ProtonProviderTest::folderUploadFailuresCleanStagingAndKeepVerifiedFiles()
{
    QFETCH(QString, failure);
    QTemporaryDir source;
    FilesystemRunner runner;
    runner.includeListingContentSize = true;
    for (const QString &name : {QString("a-good"), QString("b-bad")}) {
        QFile file(source.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("original bytes"), qint64(14));
    }
    runner.failUploadName = failure == "folder" ? "copy" : failure == "partial" ? "b-bad"
        : failure == "manifest" ? "manifest.json" : "";
    runner.truncateUploadName = failure == "verification" ? "b-bad" : "";
    BackupEngine engine;
    ProtonProvider provider(runner);
    QString manifest, error;
    BackupResult result;
    const BackupCopyMetadata metadata {"computer", "documents", "Documents", "copy", QDateTime::currentDateTimeUtc()};
    QVERIFY(!engine.backup({source.path()}, "/backups/copy", {}, metadata, provider, &manifest, &error,
        [&](const BackupProgress &progress) {
            if ((failure == "reading" || failure == "read-error")
                && progress.phase == "staging" && progress.currentFile.endsWith("b-bad")) {
                QVERIFY(QFile::remove(source.filePath("b-bad")));
                if (failure == "read-error") QVERIFY(QDir().mkpath(source.filePath("b-bad")));
            }
            if (progress.finalizing && !runner.uploadedPaths.isEmpty()) {
                QVERIFY(!QFileInfo::exists(QFileInfo(runner.uploadedPaths.first()).path()));
            }
        }, &result, {true}));
    const auto cleanup = qScopeGuard([&] {
        if (!manifest.isEmpty()) QDir(QFileInfo(manifest).path()).removeRecursively();
    });
    QVERIFY(!runner.uploadedPaths.isEmpty());
    QVERIFY(!QFileInfo::exists(QFileInfo(runner.uploadedPaths.first()).path()));
    QCOMPARE(result.manifestVerified, failure != "manifest");
    QCOMPARE(result.verifiedFiles, failure == "folder" ? 0 : failure == "manifest" ? 2 : 1);
    if (failure == "reading" || failure == "read-error") {
        QCOMPARE(result.issues.size(), 1);
        QCOMPARE(result.issues.first().path, source.filePath("b-bad"));
        QCOMPARE(result.issues.first().phase, QString("reading"));
        QVERIFY(result.issues.first().reason.startsWith("The source file could not be read:"));
        // A failed read has already created a staged file. It must be discarded
        // before recursion, even when it is empty and would otherwise upload.
        QVERIFY(!QFileInfo::exists(runner.remoteFile("/backups/copy/b-bad")));
    }
    if (failure != "manifest") {
        QVector<BackupEntry> entries;
        BackupManifestInfo info;
        QVERIFY2(BackupManifest::load(manifest, &entries, &info, &error), qPrintable(error));
        QCOMPARE(info.status, QString("incomplete"));
        QCOMPARE(entries.size(), result.verifiedFiles);
        for (const BackupEntry &entry : entries) QCOMPARE(entry.restorePath, QString("a-good"));
    }
    const int expectedPayloadUploads = failure == "folder" || failure == "partial" ? 2 : 1;
    QCOMPARE(runner.uploadedPaths.size(), expectedPayloadUploads + 1);
    QFile original(source.filePath("a-good"));
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), QByteArray("original bytes"));
}

void ProtonProviderTest::rejectsInvalidFolderUploadPaths_data()
{
    QTest::addColumn<QString>("remotePath");
    QTest::newRow("different basename") << QString("/backups/another");
    QTest::newRow("relative") << QString("backups/copy");
    QTest::newRow("root") << QString("/");
    QTest::newRow("parent traversal") << QString("/backups/../copy");
}

void ProtonProviderTest::rejectsInvalidFolderUploadPaths()
{
    QFETCH(QString, remotePath);
    QTemporaryDir staging;
    QVERIFY(QDir().mkpath(staging.filePath("copy")));
    FakeRunner runner;
    ProtonProvider provider(runner);
    QString error;
    QVERIFY(!provider.uploadDirectory(staging.filePath("copy"), remotePath, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(runner.arguments.isEmpty());
}

void ProtonProviderTest::sharedDestinationDoesNotRepeatDirectoryChecks_data()
{
    QTest::addColumn<bool>("freshCopy");
    QTest::addColumn<bool>("listingMetadata");
    QTest::addColumn<bool>("partialMetadata");
    QTest::addColumn<int>("folderCount");
    QTest::newRow("existing namespace") << false << false << false << 1;
    QTest::newRow("fresh namespace storage-only fallback") << true << false << false << 1;
    QTest::newRow("fresh namespace bulk verification") << true << true << false << 1;
    QTest::newRow("fresh namespace partial metadata") << true << true << true << 1;
    QTest::newRow("two folders bulk verification") << true << true << false << 2;
    QTest::newRow("two folders partial metadata") << true << true << true << 2;
    QTest::newRow("two folders disable unsupported bulk") << true << false << false << 2;
}

void ProtonProviderTest::sharedDestinationDoesNotRepeatDirectoryChecks()
{
    QFETCH(bool, freshCopy);
    QFETCH(bool, listingMetadata);
    QFETCH(bool, partialMetadata);
    QFETCH(int, folderCount);
    QTemporaryDir source;
    FilesystemRunner runner;
    runner.includeListingContentSize = listingMetadata;
    runner.omitListingMetadataName = partialMetadata ? "file-0.txt" : "";
    for (int folder = 0; folder < folderCount; ++folder) {
        const QString parent = folderCount == 1 ? source.path() : source.filePath(QString("folder-%1").arg(folder));
        QVERIFY(QDir().mkpath(parent));
        for (int i = 0; i < 100 / folderCount; ++i) {
            QFile file(QDir(parent).filePath(QString("file-%1.txt").arg(i)));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("payload"), qint64(7));
        }
    }
    BackupEngine engine;
    ProtonProvider provider(runner);
    QString manifest, error;
    QVERIFY2(engine.backup({source.path()}, "/backups/copy", {}, {}, provider,
        &manifest, &error, {}, nullptr, {freshCopy}), qPrintable(error));
    const auto cleanup = qScopeGuard([&] { QDir(QFileInfo(manifest).absolutePath()).removeRecursively(); });
    int lists = 0, inspections = 0, uploads = 0;
    for (const auto &call : runner.calls) {
        lists += call.at(1) == "list";
        inspections += call.at(1) == "info";
        uploads += call.at(1) == "upload";
    }
    const int parentChecks = (freshCopy || folderCount == 1) ? 2 : 2 + folderCount;
    QCOMPARE(lists, parentChecks + (freshCopy ? listingMetadata ? folderCount : 1 : 0));
    QCOMPARE(uploads, freshCopy ? 2 : 101);
    QCOMPARE(inspections, !freshCopy ? 201 : !listingMetadata ? 101 : partialMetadata ? folderCount + 1 : 1);
    QVector<BackupEntry> entries;
    QVERIFY(BackupManifest::load(manifest, &entries));
    QCOMPARE(entries.size(), 100);
}

void ProtonProviderTest::freshCopyVerificationRejectsChangedPayloads_data()
{
    QTest::addColumn<QString>("damage");
    QTest::addColumn<bool>("bulk");
    for (bool bulk : {false, true}) {
        for (const QString &damage : {QString("removed"), QString("truncated"), QString("corrupted"), QString("listing failure")}) {
            QTest::newRow(qPrintable(damage + (bulk ? " bulk" : " individual"))) << damage << bulk;
        }
    }
}

void ProtonProviderTest::freshCopyVerificationRejectsChangedPayloads()
{
    QFETCH(QString, damage);
    QFETCH(bool, bulk);
    class ChangingRunner final : public ProcessRunner {
    public:
        FilesystemRunner filesystem;
        QString damage;
        ProcessOutput run(const QStringList &arguments) override
        {
            const auto result = filesystem.run(arguments);
            // Change the first uploaded payload after the last upload. Only
            // fresh post-upload metadata may establish manifest eligibility.
            if (arguments.at(1) == "upload" && QFileInfo(arguments.at(arguments.size() - 2)).isDir()) {
                const QString path = filesystem.remoteFile("/backups/copy/a-bad");
                if (damage == "removed") {
                    QFile::remove(path);
                } else if (damage == "listing failure") {
                    filesystem.failListPath = "/backups/copy";
                } else {
                    QFile file(path);
                    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                    if (!file.open(QIODevice::WriteOnly)) return {1, {}, "Fixture mutation failed"};
                    file.write(damage == "truncated" ? "x" : "corrupt");
                }
            }
            return result;
        }
    } runner;
    runner.damage = damage;
    runner.filesystem.includeListingContentSize = bulk;
    runner.filesystem.includeSha256 = true;
    QTemporaryDir source;
    for (const QString &name : {QString("a-bad"), QString("z-good")}) {
        QFile file(source.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("payload"), qint64(7));
    }
    BackupEngine engine;
    ProtonProvider provider(runner);
    const BackupCopyMetadata metadata {"computer", "documents", "Documents", "copy", QDateTime::currentDateTimeUtc()};
    QString manifest, error;
    BackupResult result;
    QVector<BackupProgress> samples;
    const bool success = damage == "listing failure";
    QCOMPARE(engine.backup({source.path()}, "/backups/copy", {}, metadata, provider, &manifest, &error,
        [&](const BackupProgress &progress) { samples.append(progress); }, &result, {true}), success);
    QVERIFY(!manifest.isEmpty());
    const auto cleanup = qScopeGuard([&] { QDir(QFileInfo(manifest).path()).removeRecursively(); });
    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    QVERIFY2(BackupManifest::load(manifest, &entries, &info, &error), qPrintable(error));
    QCOMPARE(entries.size(), success ? 2 : 1);
    QCOMPARE(result.verifiedFiles, entries.size());
    QCOMPARE(result.verifiedBytes, qint64(entries.size() * 7));
    QVERIFY(result.manifestVerified);
    QCOMPARE(info.status, success ? QString("complete") : QString("incomplete"));
    QCOMPARE(result.issues.size(), success ? 0 : 1);
    if (!success) {
        QCOMPARE(info.failedItems, QStringList {"a-bad"});
        QCOMPARE(entries.first().restorePath, QString("z-good"));
        QCOMPARE(result.issues.first().phase, QString("verifying"));
    }
    QCOMPARE(samples.last().processedFiles, 2);
    QCOMPARE(samples.last().verifiedFiles, entries.size());
    for (const BackupProgress &sample : samples) {
        if (sample.phase == "uploading-folder") QCOMPARE(sample.verifiedFiles, 0);
    }
    for (const QString &path : runner.filesystem.uploadedPaths) {
        if (path != manifest) QVERIFY(!QFileInfo::exists(path));
    }
}

void ProtonProviderTest::nestedDirectoriesShareAncestorsOnlyWithinOperation_data()
{
    QTest::addColumn<bool>("failManifest");
    QTest::newRow("successful operation") << false;
    QTest::newRow("early failure") << true;
}

void ProtonProviderTest::nestedDirectoriesShareAncestorsOnlyWithinOperation()
{
    QFETCH(bool, failManifest);
    QTemporaryDir source;
    FilesystemRunner runner;
    for (const QString &path : {QString("src/components/one"), QString("src/pages/two")}) {
        QVERIFY(QDir().mkpath(QFileInfo(source.filePath(path)).path()));
        QFile file(source.filePath(path));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("payload");
    }
    runner.failUploadName = failManifest ? "manifest.json" : "";
    BackupEngine engine;
    ProtonProvider provider(runner);
    QString manifest, error;
    QCOMPARE(engine.backup(source.path(), "/backups/copy", provider, &manifest, &error), !failManifest);
    const auto cleanup = qScopeGuard([&] {
        if (!manifest.isEmpty()) QDir(QFileInfo(manifest).path()).removeRecursively();
    });
    QStringList listed;
    for (const auto &call : runner.calls) if (call.at(1) == "list") listed.append(call.last());
    QCOMPARE(listed, (QStringList {"/backups", "/backups/copy", "/backups/copy/src",
        "/backups/copy/src/components", "/backups/copy/src/pages"}));

    // Operation completion (also an early return) must stop trusting ancestors.
    QVERIFY(QDir(runner.remoteFile("/backups")).removeRecursively());
    runner.calls.clear();
    QVERIFY2(provider.ensureDirectory("/backups/copy/src/pages", &error), qPrintable(error));
    QVERIFY(QFileInfo(runner.remoteFile("/backups/copy/src/pages")).isDir());
    listed.clear();
    for (const auto &call : runner.calls) if (call.at(1) == "list") listed.append(call.last());
    QCOMPARE(listed, (QStringList {"/backups", "/backups/copy", "/backups/copy/src", "/backups/copy/src/pages"}));
}

void ProtonProviderTest::failedUploadRechecksMissingAncestors()
{
    class RemovingRunner final : public ProcessRunner {
    public:
        FilesystemRunner filesystem;
        bool removed = false;
        ProcessOutput run(const QStringList &arguments) override
        {
            if (!removed && arguments.at(1) == "upload") {
                removed = true;
                QDir(filesystem.remoteFile("/backups")).removeRecursively();
            }
            return filesystem.run(arguments);
        }
    } runner;
    QTemporaryDir source;
    QVERIFY(QDir().mkpath(source.filePath("nested")));
    QFile file(source.filePath("nested/one"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("payload");
    file.close();
    BackupEngine engine;
    ProtonProvider provider(runner);
    QString manifest, error;
    QVERIFY2(engine.backup({source.path()}, "/backups/copy", {}, {}, provider,
        &manifest, &error, {}, nullptr, {true}), qPrintable(error));
    const auto cleanup = qScopeGuard([&] { QDir(QFileInfo(manifest).path()).removeRecursively(); });
    QStringList listed;
    for (const auto &call : runner.filesystem.calls) if (call.at(1) == "list") listed.append(call.last());
    QCOMPARE(listed, (QStringList {"/backups", "/backups/copy",
        "/backups", "/backups/copy", "/backups/copy/nested"}));
    QVector<BackupEntry> entries;
    QVERIFY(BackupManifest::load(manifest, &entries));
    QCOMPARE(entries.size(), 1);
    QFile uploaded(runner.filesystem.remoteFile(entries.first().remotePath));
    QVERIFY(uploaded.open(QIODevice::ReadOnly));
    QCOMPARE(QCryptographicHash::hash(uploaded.readAll(), QCryptographicHash::Sha256), entries.first().checksum);
}

void ProtonProviderTest::uploadUsesJsonCliArguments()
{
    FakeRunner runner;
    runner.response.exitCode = 0;
    ProtonProvider provider(runner);

    QVERIFY(provider.upload(QStringLiteral("/tmp/file.txt"), QStringLiteral("/backups/file.txt")));
    const QStringList expected {
        QStringLiteral("filesystem"), QStringLiteral("upload"), QStringLiteral("-j"),
        QStringLiteral("-f"), QStringLiteral("replace"), QStringLiteral("-d"), QStringLiteral("replace"),
        QStringLiteral("-t"), QStringLiteral("/tmp/file.txt"), QStringLiteral("/backups"),
    };

    QCOMPARE(runner.arguments, expected);
}

void ProtonProviderTest::verifiesCopiesUsingBulkMetadataWithIndividualFallback_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<int>("listCount");
    QTest::addColumn<int>("inspectCount");
    QTest::addColumn<bool>("checksum");
    for (bool checksum : {false, true}) {
        const QString suffix = checksum ? " checksummed" : " size only";
        QTest::newRow(qPrintable("content metadata" + suffix)) << QString("full") << 2 << 0 << checksum;
        QTest::newRow(qPrintable("partial content metadata" + suffix)) << QString("partial") << 2 << 2 << checksum;
        QTest::newRow(qPrintable("storage metadata only" + suffix)) << QString("storage") << 1 << 100 << checksum;
        QTest::newRow(qPrintable("failed listing" + suffix)) << QString("failed") << 1 << 100 << checksum;
    }
}

void ProtonProviderTest::verifiesCopiesUsingBulkMetadataWithIndividualFallback()
{
    QFETCH(QString, mode);
    QFETCH(int, listCount);
    QFETCH(int, inspectCount);
    QFETCH(bool, checksum);
    QTemporaryDir source;
    FilesystemRunner runner;
    for (const QString &folder : {QString("one"), QString("two")}) {
        QVERIFY(QDir().mkpath(source.filePath(folder)));
        for (int i = 0; i < 50; ++i) {
            QFile file(source.filePath(QString("%1/file-%2").arg(folder).arg(i)));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("payload");
        }
    }
    ProtonProvider provider(runner);
    BackupEngine engine;
    QString manifest, error;
    const BackupCopyMetadata metadata {"computer", "documents", "Documents", "copy", QDateTime::currentDateTimeUtc()};
    QVERIFY2(engine.backup({source.path()}, "/backups/copy", {}, metadata, provider,
        &manifest, &error, {}, nullptr, {true}), qPrintable(error));
    const auto cleanup = qScopeGuard([&] { QDir(QFileInfo(manifest).path()).removeRecursively(); });
    runner.calls.clear();
    runner.includeListingContentSize = mode != "storage";
    runner.includeSha256 = checksum;
    runner.omitListingMetadataName = mode == "partial" ? "file-0" : "";
    runner.failListPath = mode == "failed" ? "/backups/copy/one" : "";
    RemoteCopy copy;
    QVERIFY2(BackupCatalog::verifyCopy(provider, "/backups/copy", "documents", &copy, &error), qPrintable(error));
    QCOMPARE(copy.entries.size(), 100);
    QVERIFY(copy.complete());
    int lists = 0, inspections = 0;
    for (const auto &call : runner.calls) {
        lists += call.at(1) == "list";
        inspections += call.at(1) == "info";
    }
    QCOMPARE(lists, listCount);
    QCOMPARE(inspections, inspectCount);
}

void ProtonProviderTest::bulkMetadataIgnoresUnsafeAndUnverifiableEntries()
{
    FakeRunner runner;
    runner.response = {0, QStringLiteral(R"([
        {"name":"valid","type":"file","totalStorageSize":513,"activeRevision":{"claimedSize":17}},
        {"name":"empty","type":"file","size":0},
        {"name":"storage-only","type":"file","totalStorageSize":17},
        {"name":"negative","type":"file","size":-1},
        {"name":"fractional","type":"file","size":1.5},
        {"name":"oversized","type":"file","size":1e30},
        {"name":"bad-sha","type":"file","size":17,"sha256":"invalid"},
        {"name":"bad-sha-type","type":"file","size":17,"sha256":123},
        {"name":"folder","type":"folder","size":17},
        {"name":"escape","path":"/elsewhere/escape","type":"file","size":17},
        {"name":"../traversal","type":"file","size":17},
        {"name":"duplicate","type":"file","size":17},
        {"name":"duplicate","type":"file","size":18}
    ])"), {}};
    ProtonProvider provider(runner);
    QVector<RemoteFile> files;
    QVERIFY(provider.inspectDirectoryFiles("/backups/copy", &files));
    QCOMPARE(files.size(), 2);
    QCOMPARE(files.first().path, QString("/backups/copy/valid"));
    QCOMPARE(files.first().size, qint64(17));
    QCOMPARE(files.last().size, qint64(0));
}

void ProtonProviderTest::bulkVerificationPreservesRetentionAndCancellationGates_data()
{
    QTest::addColumn<bool>("checksum");
    QTest::addColumn<QByteArray>("damagedContent");
    QTest::newRow("checksum mismatch") << true << QByteArray("corrupt");
    QTest::newRow("size mismatch with checksum") << true << QByteArray("x");
    QTest::newRow("size mismatch without checksum") << false << QByteArray("x");
}

void ProtonProviderTest::bulkVerificationPreservesRetentionAndCancellationGates()
{
    QFETCH(bool, checksum);
    QFETCH(QByteArray, damagedContent);
    QTemporaryDir source;
    FilesystemRunner runner;
    QFile file(source.filePath("one"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("payload");
    file.close();
    ProtonProvider provider(runner);
    BackupEngine engine;
    QString manifest, error;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const QString &id : {QString("old"), QString("new")}) {
        const BackupCopyMetadata metadata {"computer", "documents", "Documents", id, id == "old" ? now.addDays(-1) : now};
        QVERIFY(engine.backup({source.path()}, "/backups/" + id, {}, metadata, provider, &manifest, &error));
        QDir(QFileInfo(manifest).path()).removeRecursively();
    }
    runner.includeListingContentSize = true;
    runner.includeSha256 = checksum;
    QFile corrupted(runner.remoteFile("/backups/new/one"));
    QVERIFY(corrupted.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QVERIFY(corrupted.open(QIODevice::WriteOnly));
    QCOMPARE(corrupted.write(damagedContent), qint64(damagedContent.size()));
    corrupted.close();
    QVector<RemoteCopy> copies;
    QVERIFY(BackupCatalog::discoverCopies(provider, "/backups", "documents", &copies, &error));
    QCOMPARE(copies.size(), 2);
    QVERIFY(!copies.first().complete());
    QCOMPARE(copies.first().unavailableItems, QStringList {"one"});
    QVERIFY(BackupCleanup::eligibleTargets(copies, 1, "computer", "documents").isEmpty());

    runner.calls.clear();
    RemoteCopy copy;
    int cancellationChecks = 0;
    QVERIFY(!BackupCatalog::verifyCopy(provider, "/backups/old", "documents", &copy, &error,
        [&] { return ++cancellationChecks == 4; }));
    QCOMPARE(runner.calls.size(), 2); // Manifest download and one directory listing.
    QVERIFY(copy.entries.isEmpty());
}

void ProtonProviderTest::uploadsAtExactRequestedPath_data()
{
    QTest::addColumn<QString>("remotePath");
    QTest::newRow("unchanged basename") << QStringLiteral("/backups/source.txt");
    QTest::newRow("renamed basename") << QStringLiteral("/backups/renamed.txt");
    QTest::newRow("nested folder and spaces") << QStringLiteral("/backups/nested folder/renamed file.txt");
    QTest::newRow("root destination") << QStringLiteral("/renamed.txt");
}

void ProtonProviderTest::uploadsAtExactRequestedPath()
{
    QFETCH(QString, remotePath);
    QTemporaryDir source;
    FilesystemRunner runner;
    QVERIFY(source.isValid() && runner.remote.isValid());
    QFile file(source.filePath(QStringLiteral("source.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray contents("payload\0with binary bytes", 25);
    QCOMPARE(file.write(contents), contents.size());
    file.close();
    QVERIFY(QDir().mkpath(QFileInfo(runner.remoteFile(remotePath)).path()));
    ProtonProvider provider(runner);
    QString error;
    QVERIFY2(provider.upload(file.fileName(), remotePath, &error), qPrintable(error));

    QFile uploaded(runner.remoteFile(remotePath));
    QVERIFY(uploaded.open(QIODevice::ReadOnly));
    QCOMPARE(uploaded.readAll(), contents);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), contents);
    QCOMPARE(runner.uploadedPaths.size(), 1);
    const QString cliPath = runner.uploadedPaths.first();
    QCOMPARE(QFileInfo(cliPath).fileName(), QFileInfo(remotePath).fileName());
    if (remotePath.endsWith(QStringLiteral("/source.txt"))) {
        QCOMPARE(cliPath, file.fileName());
    } else {
        QVERIFY(!QFileInfo::exists(QFileInfo(cliPath).path()));
        QVERIFY(!QFileInfo::exists(QDir(QFileInfo(uploaded.fileName()).path()).filePath("source.txt")));
    }
}

void ProtonProviderTest::rejectsInvalidUploadPaths_data()
{
    QTest::addColumn<QString>("remotePath");
    QTest::newRow("empty") << QString();
    QTest::newRow("relative") << QStringLiteral("backups/file.txt");
    QTest::newRow("root") << QStringLiteral("/");
    QTest::newRow("folder") << QStringLiteral("/backups/");
    QTest::newRow("dot") << QStringLiteral("/backups/.");
    QTest::newRow("parent") << QStringLiteral("/backups/../file.txt");
}

void ProtonProviderTest::rejectsInvalidUploadPaths()
{
    QFETCH(QString, remotePath);
    FakeRunner runner;
    ProtonProvider provider(runner);
    QString error;
    QVERIFY(!provider.upload(QStringLiteral("/tmp/source.txt"), remotePath, &error));
    QCOMPARE(error, QStringLiteral("The provider upload path is invalid."));
    QVERIFY(runner.arguments.isEmpty());
}

void ProtonProviderTest::stagingFailureDoesNotUpload()
{
    QTemporaryDir source;
    QVERIFY(source.isValid());
    FakeRunner runner;
    ProtonProvider provider(runner);
    QString error;
    QVERIFY(!provider.upload(source.filePath("missing.txt"), QStringLiteral("/backups/renamed.txt"), &error));
    QVERIFY(error.startsWith(QStringLiteral("The file could not be staged for Proton Drive upload:")));
    QVERIFY(runner.arguments.isEmpty());
}

void ProtonProviderTest::uploadFailurePreservesFilesAndCleansStaging()
{
    QTemporaryDir source;
    FilesystemRunner runner;
    QVERIFY(source.isValid() && runner.remote.isValid());
    QFile file(source.filePath("manifest.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("source payload");
    file.close();
    QVERIFY(QDir().mkpath(runner.remoteFile("/backups")));
    QFile existing(runner.remoteFile("/backups/manifest.json"));
    QVERIFY(existing.open(QIODevice::WriteOnly));
    existing.write("existing application manifest");
    existing.close();
    runner.failUploadName = QStringLiteral("manifest.json.1");
    ProtonProvider provider(runner);
    QString error;
    QVERIFY(!provider.upload(file.fileName(), QStringLiteral("/backups/manifest.json.1"), &error));
    QCOMPARE(error, QStringLiteral("Connection interrupted"));
    QCOMPARE(runner.uploadedPaths.size(), 1);
    QVERIFY(!QFileInfo::exists(QFileInfo(runner.uploadedPaths.first()).path()));
    QVERIFY(!QFileInfo::exists(runner.remoteFile("/backups/manifest.json.1")));
    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), QByteArray("existing application manifest"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("source payload"));
}

void ProtonProviderTest::backsUpAndRestoresReservedAndCollisionNames()
{
    QTemporaryDir source;
    QTemporaryDir restored;
    FilesystemRunner runner;
    QVERIFY(source.isValid() && restored.isValid() && runner.remote.isValid());
    const QStringList names {"manifest.json", "manifest.json.1", "manifest.json.1.1"};
    for (const QString &name : names) {
        QFile file(source.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray contents = (QStringLiteral("payload for ") + name).toUtf8();
        QCOMPARE(file.write(contents), contents.size());
    }
    BackupEngine engine;
    ProtonProvider provider(runner);
    QString manifest;
    QString error;
    QVERIFY2(engine.backup(source.path(), QStringLiteral("/backups/copy"), provider, &manifest, &error), qPrintable(error));
    const auto cleanupManifest = qScopeGuard([&] { QDir(QFileInfo(manifest).path()).removeRecursively(); });
    QVector<BackupEntry> entries;
    QVERIFY2(BackupManifest::load(manifest, &entries, &error), qPrintable(error));
    QCOMPARE(entries.size(), names.size());
    QFile applicationManifest(runner.remoteFile("/backups/copy/manifest.json"));
    QVERIFY(applicationManifest.open(QIODevice::ReadOnly));
    QVERIFY(QJsonDocument::fromJson(applicationManifest.readAll()).isObject());
    for (qsizetype index = 0; index < names.size(); ++index) {
        const QString name = names.at(index);
        const BackupEntry entry = entries.at(index);
        QCOMPARE(entry.sourcePath, source.filePath(name));
        QCOMPARE(entry.remotePath, QStringLiteral("/backups/copy/") + name + QStringLiteral(".1"));
        const QByteArray expected = (QStringLiteral("payload for ") + name).toUtf8();
        QFile uploaded(runner.remoteFile(entry.remotePath));
        QVERIFY(uploaded.open(QIODevice::ReadOnly));
        QCOMPARE(uploaded.readAll(), expected);
        QVERIFY2(engine.restoreFile(entry, restored.path(), provider, &error), qPrintable(error));
        QFile restoredFile(restored.filePath(name));
        QVERIFY(restoredFile.open(QIODevice::ReadOnly));
        QCOMPARE(restoredFile.readAll(), expected);
        QFile original(source.filePath(name));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), expected);
    }
    QCOMPARE(QDir(runner.remoteFile("/backups/copy")).entryList(QDir::Files).size(), 4);
    for (const QString &path : runner.uploadedPaths) {
        if (!path.startsWith(source.path() + '/')) {
            QVERIFY(!QFileInfo::exists(QFileInfo(path).path()) || path == manifest);
        }
    }
}

void ProtonProviderTest::failedPayloadsAreNotVerified_data()
{
    QTest::addColumn<bool>("truncateUpload");
    QTest::newRow("CLI upload failure") << false;
    QTest::newRow("remote verification failure") << true;
}

void ProtonProviderTest::failedPayloadsAreNotVerified()
{
    QFETCH(bool, truncateUpload);
    QTemporaryDir source;
    FilesystemRunner runner;
    QVERIFY(source.isValid() && runner.remote.isValid());
    for (const QString &name : {QString("good.txt"), QString("manifest.json")}) {
        QFile file(source.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("important payload");
    }
    if (truncateUpload) {
        runner.truncateUploadName = QStringLiteral("manifest.json.1");
    } else {
        runner.failUploadName = QStringLiteral("manifest.json.1");
    }
    BackupEngine engine;
    ProtonProvider provider(runner);
    QString manifest;
    QString error;
    const BackupCopyMetadata metadata {"computer", "set", "Documents", "copy", QDateTime::currentDateTimeUtc()};
    QVERIFY(!engine.backup({source.path()}, QStringLiteral("/backups/copy"), {}, metadata,
        provider, &manifest, &error, {}, nullptr, {true}));
    QVERIFY(!manifest.isEmpty());
    const auto cleanupManifest = qScopeGuard([&] { QDir(QFileInfo(manifest).path()).removeRecursively(); });
    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    QVERIFY2(BackupManifest::load(manifest, &entries, &info, &error), qPrintable(error));
    QCOMPARE(info.status, QStringLiteral("incomplete"));
    QCOMPARE(info.failedItems, QStringList {QStringLiteral("manifest.json")});
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().remotePath, QStringLiteral("/backups/copy/good.txt"));
    QFile good(runner.remoteFile(entries.first().remotePath));
    QVERIFY(good.open(QIODevice::ReadOnly));
    QCOMPARE(good.readAll(), QByteArray("important payload"));
    QFile remoteManifest(runner.remoteFile("/backups/copy/manifest.json"));
    QVERIFY(remoteManifest.open(QIODevice::ReadOnly));
    QFile localManifest(manifest);
    QVERIFY(localManifest.open(QIODevice::ReadOnly));
    QCOMPARE(remoteManifest.readAll(), localManifest.readAll());
}

void ProtonProviderTest::inspectParsesVerifiedMetadata()
{
    FakeRunner runner;
    runner.response.exitCode = 0;
    runner.response.standardOutput = QStringLiteral(R"({"size":17,"sha256":"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"})");
    ProtonProvider provider(runner);
    RemoteFile file;

    QVERIFY(provider.inspect(QStringLiteral("/backups/file.txt"), &file));
    QCOMPARE(file.path, QStringLiteral("/backups/file.txt"));
    QCOMPARE(file.size, qint64(17));
    QCOMPARE(file.checksum, QByteArray::fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"));
}

void ProtonProviderTest::inspectParsesCliMetadataWithoutSha256()
{
    FakeRunner runner;
    runner.response.exitCode = 0;
    runner.response.standardOutput = QStringLiteral(R"({"type":"file","totalStorageSize":17,"activeRevision":{"claimedSize":17}})");
    ProtonProvider provider(runner);
    RemoteFile file;

    QVERIFY(provider.inspect(QStringLiteral("/backups/file.txt"), &file));
    QCOMPARE(file.size, qint64(17));
    QVERIFY(file.checksum.isEmpty());
}

void ProtonProviderTest::inspectUsesContentSizeInsteadOfEncryptedStorageSize()
{
    FakeRunner runner;
    runner.response.exitCode = 0;
    runner.response.standardOutput = QStringLiteral(R"({"type":"file","totalStorageSize":463208,"activeRevision":{"storageSize":463208,"claimedSize":463105}})");
    ProtonProvider provider(runner);
    RemoteFile file;

    QVERIFY(provider.inspect(QStringLiteral("/backups/file.pdf"), &file));
    QCOMPARE(file.size, qint64(463105));
}

void ProtonProviderTest::inspectRejectsStorageSizeWithoutContentSize()
{
    FakeRunner runner;
    runner.response.exitCode = 0;
    runner.response.standardOutput = QStringLiteral(R"({"type":"file","totalStorageSize":513,"activeRevision":{"storageSize":513}})");
    ProtonProvider provider(runner);
    RemoteFile file;
    QString error;

    QVERIFY(!provider.inspect(QStringLiteral("/backups/manifest.json"), &file, &error));
    QCOMPARE(error, QStringLiteral("Proton Drive returned invalid file metadata."));
}

void ProtonProviderTest::successfulInspectClearsEarlierErrors()
{
    FakeRunner runner;
    runner.response.exitCode = 1;
    runner.response.standardError = QStringLiteral("Node not found: file.txt");
    ProtonProvider provider(runner);
    RemoteFile file;
    QString error;

    QVERIFY(!provider.inspect(QStringLiteral("/backups/file.txt"), &file, &error));
    QCOMPARE(error, QStringLiteral("Node not found: file.txt"));

    runner.response = {0, QStringLiteral(R"({"activeRevision":{"claimedSize":17}})"), {}};
    QVERIFY(provider.inspect(QStringLiteral("/backups/file.txt"), &file, &error));
    QVERIFY(error.isEmpty());
}

void ProtonProviderTest::commandErrorsAreActionable()
{
    FakeRunner runner;
    runner.response.exitCode = 1;
    runner.response.standardError = QStringLiteral("not authenticated");
    ProtonProvider provider(runner);
    QString error;

    QVERIFY(!provider.download(QStringLiteral("/remote/file"), QStringLiteral("/tmp/file"), &error));
    QCOMPARE(error, QStringLiteral("not authenticated"));
}

void ProtonProviderTest::rejectsNullMetadataOutput()
{
    FakeRunner runner;
    ProtonProvider provider(runner);
    QString error;

    QVERIFY(!provider.inspect(QStringLiteral("/remote/file"), nullptr, &error));
    QCOMPARE(error, QStringLiteral("A destination for remote file metadata is required."));
    QVERIFY(runner.arguments.isEmpty());
}

void ProtonProviderTest::listsRemoteItemsAndUsesExactCleanupCommands()
{
    FakeRunner runner;
    runner.response.exitCode = 0;
    runner.response.standardOutput = QStringLiteral(R"([{"name":{"ok":true,"value":"computer"},"type":"folder"},{"name":{"ok":true,"value":"notes.txt"},"type":"file","totalStorageSize":5,"modificationTime":"2026-09-28T12:00:00.000Z"}])");
    ProtonProvider provider(runner);
    QVector<RemoteItem> items;
    QVERIFY(provider.list(QStringLiteral("/my-files/backups"), &items));
    QCOMPARE(items.size(), 2);
    QVERIFY(items.first().directory);
    QCOMPARE(items.last().path, QStringLiteral("/my-files/backups/notes.txt"));

    QVERIFY(provider.trash(QStringLiteral("/my-files/backups/computer/set/copy")));
    const QStringList trashArguments {
        QStringLiteral("filesystem"), QStringLiteral("trash"), QStringLiteral("/my-files/backups/computer/set/copy"),
    };
    QCOMPARE(runner.arguments, trashArguments);
    QVERIFY(provider.permanentlyDelete(QStringLiteral("/my-files/backups/computer/set/copy")));
    const QStringList deleteArguments {
        QStringLiteral("filesystem"), QStringLiteral("delete"), QStringLiteral("/my-files/backups/computer/set/copy"),
    };
    QCOMPARE(runner.arguments, deleteArguments);
}

QTEST_MAIN(ProtonProviderTest)
#include "protonprovider_test.moc"

#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopeGuard>

#include "../src/backupengine.h"
#include "../src/backupmanifest.h"
#include "../src/localprovider.h"

class FailingProvider final : public BackupProvider
{
public:
    explicit FailingProvider(const QString &root) : local(root), localRoot(root) {}
    LocalProvider local;
    QString localRoot;
    bool failManifest = false;
    bool failAll = false;
    bool omitChecksum = false;
    QStringList uploadedPayloads;
    QStringList ensuredPaths;
    bool removeDirectoryOnUpload = false;
    bool upload(const QString &source, const QString &remote, QString *error) override
    {
        if (!remote.endsWith("manifest.json")) uploadedPayloads.append(source);
        if (removeDirectoryOnUpload) {
            removeDirectoryOnUpload = false;
            QDir(localRoot + "/" + QFileInfo(remote).path()).removeRecursively();
            if (error) *error = QStringLiteral("The destination folder disappeared.");
            return false;
        }
        if (remote.endsWith("bad-upload") || (failManifest && remote.endsWith("manifest.json"))
            || (failAll && !remote.endsWith("manifest.json"))) {
            if (error) *error = QStringLiteral("Connection interrupted");
            return false;
        }
        return local.upload(source, remote, error);
    }
    bool inspect(const QString &path, RemoteFile *file, QString *error) override
    {
        if (!local.inspect(path, file, error)) return false;
        if (path.endsWith("bad-verify")) ++file->size;
        if (omitChecksum) file->checksum.clear();
        return true;
    }
    bool ensureDirectory(const QString &path, QString *error) override
    {
        ensuredPaths.append(path);
        return local.ensureDirectory(path, error);
    }
    bool download(const QString &path, const QString &destination, QString *error) override { return local.download(path, destination, error); }
    bool list(const QString &path, QVector<RemoteItem> *items, QString *error) override { return local.list(path, items, error); }
    bool trash(const QString &path, QString *error) override { return local.trash(path, error); }
    bool permanentlyDelete(const QString &path, QString *error) override { return local.permanentlyDelete(path, error); }
};

class BackupEngineTest final : public QObject
{
    Q_OBJECT

private slots:
    void rejectsMissingSource();
    void rejectsUnsafeRemoteRoot();
    void listsRegularFilesAndSkipsSymlinks();
    void backsUpAndRestoresHiddenContents_data();
    void backsUpAndRestoresHiddenContents();
    void appliesExclusionsToHiddenContents();
    void skipsHiddenSymbolicLinksUnlessExcluded();
    void backsUpVerifiesAndRestoresOneFile();
    void backsUpStoresVerifiedChecksum();
    void sourceChangesAfterHashingRestoreStagedContent_data();
    void sourceChangesAfterHashingRestoreStagedContent();
    void sizeOnlyMetadataDoesNotReuseDifferentContent();
    void verifiesTransferredPayloadMetadata_data();
    void verifiesTransferredPayloadMetadata();
    void reservesManifestPathForSourceFiles();
    void previewsMultipleSourcesAndExclusions();
    void cancelledPreviewStopsTraversalAndDiscardsPartialResults();
    void excludesMatchingFolderNamesAtEveryDepth_data();
    void excludesMatchingFolderNamesAtEveryDepth();
    void absoluteExclusionDoesNotExcludeSameNamedFoldersElsewhere();
    void backsUpMultipleSourcesWithoutCollisions();
    void preservesVerifiedItemsInAnIncompleteCopy();
    void retriesOnlyReuseMatchingChecksummedPayloads_data();
    void retriesOnlyReuseMatchingChecksummedPayloads();
    void localProviderRejectsUnsafePaths();
    void reportsProgressForIncludedFilesAndFinalization_data();
    void reportsProgressForIncludedFilesAndFinalization();
    void reportsFailuresAndKeepsOnlyVerifiedFilesRestorable_data();
    void reportsFailuresAndKeepsOnlyVerifiedFilesRestorable();
    void ensuresEachPayloadDirectoryOnceAndRechecksFailedUploads();
    void progressPersistenceBoundsWritesAndKeepsFinalSamples();
};

void BackupEngineTest::ensuresEachPayloadDirectoryOnceAndRechecksFailedUploads()
{
    QTemporaryDir source, remote;
    QVERIFY(QDir().mkpath(source.filePath("nested")));
    for (int i = 0; i < 100; ++i) {
        QFile file(source.filePath(QString("nested/file-%1").arg(i)));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("payload"), qint64(7));
    }
    BackupEngine engine;
    FailingProvider provider(remote.path());
    QString manifest, error;
    QVERIFY2(engine.backup({source.path()}, "copy", {}, provider, &manifest, &error), qPrintable(error));
    QCOMPARE(provider.ensuredPaths, (QStringList {"copy", "copy/nested"}));
    QDir(QFileInfo(manifest).absolutePath()).removeRecursively();

    provider.ensuredPaths.clear();
    provider.removeDirectoryOnUpload = true;
    QVERIFY2(engine.backup({source.path()}, "retry", {}, provider, &manifest, &error), qPrintable(error));
    QCOMPARE(provider.ensuredPaths, (QStringList {"retry", "retry/nested", "retry/nested"}));
    QVector<BackupEntry> entries;
    QVERIFY(BackupManifest::load(manifest, &entries));
    QCOMPARE(entries.size(), 100);
    for (const BackupEntry &entry : entries) {
        RemoteFile file;
        QVERIFY(provider.inspect(entry.remotePath, &file, &error));
        QCOMPARE(file.checksum, entry.checksum);
    }
    QDir(QFileInfo(manifest).absolutePath()).removeRecursively();
}

void BackupEngineTest::progressPersistenceBoundsWritesAndKeepsFinalSamples()
{
    BackupProgressPersistence persistence;
    BackupProgress progress;
    progress.totalFiles = 10000;
    int saves = 0;
    // 40,000 phase transitions in a simulated ten-second run must not produce
    // 40,000 state commits. Elapsed time is deterministic, not a timing assertion.
    for (int i = 0; i < 40000; ++i) {
        progress.processedFiles = i / 4;
        progress.currentFile = QString::number(progress.processedFiles);
        progress.phase = QString::number(i % 4);
        saves += persistence.shouldSave(progress, i / 4);
    }
    QCOMPARE(saves, 10);
    progress.processedFiles = 10000;
    QVERIFY(persistence.shouldSave(progress, 9999));
    QVERIFY(!persistence.shouldSave(progress, 9999));
    progress.finalizing = true;
    QVERIFY(persistence.shouldSave(progress, 9999));
    QVERIFY(!persistence.shouldSave(progress, 9999));
    QVERIFY(persistence.shouldSave(progress, 10999));
}

void BackupEngineTest::reportsProgressForIncludedFilesAndFinalization_data()
{
    QTest::addColumn<bool>("removeFile");
    QTest::addColumn<bool>("readError");
    QTest::newRow("successful files") << false << false;
    QTest::newRow("file becomes unreadable") << true << false;
    QTest::newRow("read fails after snapshot creation") << true << true;
}

void BackupEngineTest::reportsProgressForIncludedFilesAndFinalization()
{
    QFETCH(bool, removeFile);
    QFETCH(bool, readError);
    QTemporaryDir source;
    QTemporaryDir remote;
    QVERIFY(source.isValid());
    QVERIFY(remote.isValid());
    for (const auto &name : {"one", "two", "three", "excluded"}) {
        QFile file(source.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(name == QByteArray("excluded") ? QByteArray(100, 'x') : QByteArray(10, 'x'));
    }
    BackupEngine engine;
    LocalProvider provider(remote.path());
    QVector<BackupProgress> updates;
    QString error;
    QString manifest;
    BackupResult result;
    const bool success = engine.backup({source.path()}, "copy", {source.filePath("excluded")}, {},
        provider, &manifest, &error, [&](const BackupProgress &progress) {
            if (updates.isEmpty() && removeFile) {
                QVERIFY(QFile::remove(source.filePath("one")));
                // Opening a directory for reading succeeds on Linux, but the
                // first read fails after the staging operation creates a file.
                if (readError) QVERIFY(QDir().mkpath(source.filePath("one")));
            }
            if (progress.finalizing) {
                QVERIFY(!QFile::exists(remote.filePath("copy/manifest.json")));
            }
            updates.append(progress);
        }, &result);
    QCOMPARE(success, !removeFile);
    QCOMPARE(updates.first().totalFiles, 3);
    QCOMPARE(updates.first().totalBytes, qint64(30));
    QCOMPARE(updates.first().processedFiles, 0);
    QVector<BackupProgress> finished;
    bool sawUpload = false;
    for (const BackupProgress &update : updates) {
        if (update.currentFile.isEmpty() && update.processedFiles > 0 && !update.finalizing) {
            finished.append(update);
        }
        if (update.phase == "uploading" && !update.currentFile.isEmpty()) {
            sawUpload = true;
            QCOMPARE(update.currentFileBytes, qint64(10));
            QVERIFY(update.processedFiles < update.totalFiles);
        }
    }
    QVERIFY(sawUpload);
    QCOMPARE(finished.size(), 3);
    for (int index = 0; index < 3; ++index) {
        QCOMPARE(finished.at(index).processedFiles, index + 1);
        QCOMPARE(finished.at(index).processedBytes, qint64((index + 1) * 10));
    }
    QVERIFY(updates.last().finalizing);
    QCOMPARE(updates.last().verifiedFiles, removeFile ? 2 : 3);
    QCOMPARE(updates.last().verifiedBytes, qint64(removeFile ? 20 : 30));
    QCOMPARE(updates.last().failedItems, removeFile ? 1 : 0);
    QVERIFY(result.manifestVerified);
    if (removeFile) {
        QCOMPARE(result.issues.first().path, source.filePath("one"));
        QCOMPARE(result.issues.first().phase, QString("reading"));
        QVERIFY(result.issues.first().reason.startsWith("The source file could not be read:"));
        QVERIFY(!QFileInfo::exists(remote.filePath("copy/one")));
    }
    QVector<BackupEntry> verified;
    QVERIFY(BackupManifest::load(manifest, &verified));
    QCOMPARE(verified.size(), removeFile ? 2 : 3);
}

void BackupEngineTest::reportsFailuresAndKeepsOnlyVerifiedFilesRestorable_data()
{
    QTest::addColumn<bool>("failManifest");
    QTest::addColumn<bool>("failAll");
    QTest::newRow("partial copy") << false << false;
    QTest::newRow("manifest failed") << true << false;
    QTest::newRow("all files failed") << false << true;
}

void BackupEngineTest::reportsFailuresAndKeepsOnlyVerifiedFilesRestorable()
{
    QFETCH(bool, failManifest);
    QFETCH(bool, failAll);
    QTemporaryDir source;
    QTemporaryDir remote;
    QTemporaryDir destination;
    QVERIFY(source.isValid() && remote.isValid() && destination.isValid());
    for (const QString &name : {QString("good"), QString("bad-upload"), QString("bad-verify")}) {
        QFile file(source.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("important content");
    }
    BackupEngine engine;
    FailingProvider provider(remote.path());
    provider.failManifest = failManifest;
    provider.failAll = failAll;
    const BackupCopyMetadata metadata {"computer", "documents", "Documents", "copy", QDateTime::currentDateTimeUtc()};
    BackupResult result;
    QString manifest = "previous manifest";
    QString error;
    QVERIFY(!engine.backup({source.path()}, "computer/Documents/copy", {}, metadata,
        provider, &manifest, &error, {}, &result));
    QVERIFY(result.reported);
    QCOMPARE(result.manifestVerified, !failManifest);
    QCOMPARE(result.verifiedFiles, failAll ? 0 : 1);
    QCOMPARE(result.issues.size(), failAll ? 3 : 2);
    QCOMPARE(result.issues.first().path, source.filePath("bad-upload"));
    QCOMPARE(result.issues.first().phase, QString("uploading"));
    QCOMPARE(result.issues.first().reason, QString("Connection interrupted"));
    for (const QString &path : provider.uploadedPayloads) {
        QVERIFY(!QFileInfo::exists(QFileInfo(path).path()));
    }
    if (failManifest) {
        QVERIFY(manifest.isEmpty());
        QCOMPARE(error, QString("Connection interrupted"));
        return;
    }
    QVERIFY(!manifest.isEmpty());
    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    QVERIFY2(BackupManifest::load(manifest, &entries, &info, &error), qPrintable(error));
    QCOMPARE(info.status, QString("incomplete"));
    QCOMPARE(info.expectedItems.size(), 3);
    QCOMPARE(info.failedItems.size(), result.issues.size());
    QCOMPARE(entries.size(), result.verifiedFiles);
    if (!failAll) {
        QCOMPARE(result.issues.at(1).phase, QString("verifying"));
        QVERIFY(engine.restoreFile(entries.first(), destination.path(), provider, &error));
        QFile restored(destination.filePath("good"));
        QVERIFY(restored.open(QIODevice::ReadOnly));
        QCOMPARE(restored.readAll(), QByteArray("important content"));
    }
    QFile saved(manifest);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    const auto issues = QJsonDocument::fromJson(saved.readAll()).object().value("issues").toArray();
    QCOMPARE(issues.size(), result.issues.size());
    QCOMPARE(issues.first().toObject().value("reason").toString(), QString("Connection interrupted"));
}

void BackupEngineTest::rejectsMissingSource()
{
    BackupEngine engine;
    QString error;

    QVERIFY(!engine.validateSelection(QStringLiteral("/tmp/omacustos-does-not-exist"), &error));
    QCOMPARE(error, QStringLiteral("The selected folder does not exist."));
}

void BackupEngineTest::rejectsUnsafeRemoteRoot()
{
    QTemporaryDir source;
    QTemporaryDir remote;
    QVERIFY(source.isValid());
    QVERIFY(remote.isValid());

    QFile file(source.filePath(QStringLiteral("file.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("content");
    file.close();

    BackupEngine engine;
    LocalProvider provider(remote.path());
    QString error;
    QVERIFY(!engine.backup(source.path(), QStringLiteral("../outside"), provider, nullptr, &error));
    QCOMPARE(error, QStringLiteral("The remote backup folder is invalid."));
}

void BackupEngineTest::backsUpVerifiesAndRestoresOneFile()
{
    QTemporaryDir source;
    QTemporaryDir remote;
    QTemporaryDir destination;
    QVERIFY(source.isValid());
    QVERIFY(remote.isValid());
    QVERIFY(destination.isValid());

    QFile original(source.filePath(QStringLiteral("notes with spaces.txt")));
    QVERIFY(original.open(QIODevice::WriteOnly));
    original.write("important content");
    original.close();

    BackupEngine engine;
    LocalProvider provider(remote.path());
    QString manifestPath;
    QString error;

    QVERIFY(engine.backup(source.path(), QStringLiteral("copy"), provider, &manifestPath, &error));
    QVERIFY2(QFileInfo::exists(manifestPath), qPrintable(error));

    const BackupEntry entry {
        original.fileName(),
        QStringLiteral("copy/notes with spaces.txt"),
        original.size(),
    };
    QVERIFY(engine.restoreFile(entry, destination.path(), provider, &error));

    QFile restored(QDir(destination.path()).filePath(original.fileName()));
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), QByteArray("important content"));
}

void BackupEngineTest::backsUpStoresVerifiedChecksum()
{
    QTemporaryDir source;
    QTemporaryDir remote;
    QVERIFY(source.isValid());
    QVERIFY(remote.isValid());

    QFile original(source.filePath(QStringLiteral("notes.txt")));
    QVERIFY(original.open(QIODevice::WriteOnly));
    original.write("important content");
    original.close();

    BackupEngine engine;
    LocalProvider provider(remote.path());
    QString manifestPath;
    QString error;
    QVERIFY(engine.backup(source.path(), QStringLiteral("copy"), provider, &manifestPath, &error));

    QVector<BackupEntry> entries;
    QVERIFY(BackupManifest::load(manifestPath, &entries, &error));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().checksum,
        QCryptographicHash::hash("important content", QCryptographicHash::Sha256));
}

void BackupEngineTest::sourceChangesAfterHashingRestoreStagedContent_data()
{
    QTest::addColumn<bool>("atomicReplacement");
    QTest::addColumn<bool>("freshCopy");
    QTest::addColumn<bool>("retryUpload");
    for (bool fresh : {false, true}) {
        for (bool retry : {false, true}) {
            const QString suffix = QString("%1%2").arg(fresh ? " fresh" : " reused namespace", retry ? " retry" : "");
            QTest::newRow(qPrintable("same-sized in-place edit" + suffix)) << false << fresh << retry;
            QTest::newRow(qPrintable("atomic pathname replacement" + suffix)) << true << fresh << retry;
        }
    }
}

void BackupEngineTest::sourceChangesAfterHashingRestoreStagedContent()
{
    QFETCH(bool, atomicReplacement);
    QFETCH(bool, freshCopy);
    QFETCH(bool, retryUpload);
    QTemporaryDir source;
    QTemporaryDir remote;
    QTemporaryDir destination;
    QVERIFY(source.isValid() && remote.isValid() && destination.isValid());
    const QString sourcePath = source.filePath(QStringLiteral("notes.txt"));
    QFile original(sourcePath);
    QVERIFY(original.open(QIODevice::WriteOnly));
    QCOMPARE(original.write("good"), qint64(4));
    original.close();

    BackupEngine engine;
    FailingProvider provider(remote.path());
    provider.omitChecksum = true; // Proton provides size-only metadata.
    provider.removeDirectoryOnUpload = retryUpload;
    QString manifest;
    QString error;
    bool changed = false;
    const BackupCopyMetadata metadata {"computer", "set", "Documents", "copy", QDateTime::currentDateTimeUtc()};
    const bool success = engine.backup({source.path()}, "copy", {}, metadata, provider, &manifest, &error,
        [&](const BackupProgress &progress) {
            if (progress.phase != QStringLiteral("uploading") || changed) return;
            // This callback runs after hashing and immediately before the
            // provider opens the upload pathname, reproducing the race exactly.
            if (atomicReplacement) {
                QSaveFile replacement(sourcePath);
                QVERIFY(replacement.open(QIODevice::WriteOnly));
                QCOMPARE(replacement.write("evil"), qint64(4));
                QVERIFY(replacement.commit());
            } else {
                QFile edited(sourcePath);
                QVERIFY(edited.open(QIODevice::WriteOnly));
                QCOMPARE(edited.write("evil"), qint64(4));
            }
            changed = true;
        }, nullptr, {freshCopy});
    const auto cleanupManifest = qScopeGuard([&] {
        if (!manifest.isEmpty()) QDir(QFileInfo(manifest).path()).removeRecursively();
    });
    QVERIFY(changed);
    QVERIFY2(success, qPrintable(error));
    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    QVERIFY2(BackupManifest::load(manifest, &entries, &info, &error), qPrintable(error));
    QCOMPARE(info.status, QStringLiteral("complete"));
    QCOMPARE(entries.size(), 1);
    const BackupEntry entry = entries.first();
    QCOMPARE(entry.sourcePath, sourcePath);
    QCOMPARE(entry.checksum, QCryptographicHash::hash("good", QCryptographicHash::Sha256));
    QFile uploaded(remote.filePath(entry.remotePath));
    QVERIFY(uploaded.open(QIODevice::ReadOnly));
    QCOMPARE(uploaded.readAll(), QByteArray("good"));
    QVERIFY2(engine.restoreFile(entry, destination.path(), provider, &error), qPrintable(error));
    QFile restored(destination.filePath("notes.txt"));
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), QByteArray("good"));
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), QByteArray("evil"));
    QCOMPARE(provider.uploadedPayloads.size(), retryUpload ? 2 : 1);
    if (retryUpload) QCOMPARE(provider.uploadedPayloads.at(0), provider.uploadedPayloads.at(1));
    QVERIFY(provider.uploadedPayloads.first() != sourcePath);
    QVERIFY(!QFileInfo::exists(QFileInfo(provider.uploadedPayloads.first()).path()));
}

void BackupEngineTest::sizeOnlyMetadataDoesNotReuseDifferentContent()
{
    QTemporaryDir source;
    QTemporaryDir remote;
    QTemporaryDir destination;
    QVERIFY(source.isValid() && remote.isValid() && destination.isValid());
    QFile original(source.filePath("retry.txt"));
    QVERIFY(original.open(QIODevice::WriteOnly));
    QCOMPARE(original.write("old!"), qint64(4));
    original.close();
    BackupEngine engine;
    FailingProvider provider(remote.path());
    provider.omitChecksum = true;
    QString manifest;
    QString error;
    QVERIFY2(engine.backup(source.path(), "copy", provider, &manifest, &error), qPrintable(error));
    const QString firstManifest = manifest;
    const auto cleanupManifests = qScopeGuard([&] {
        QDir(QFileInfo(firstManifest).path()).removeRecursively();
        if (manifest != firstManifest && !manifest.isEmpty()) QDir(QFileInfo(manifest).path()).removeRecursively();
    });
    QVERIFY(original.open(QIODevice::WriteOnly));
    QCOMPARE(original.write("new!"), qint64(4));
    original.close();
    QVERIFY2(engine.backup(source.path(), "copy", provider, &manifest, &error), qPrintable(error));
    QVector<BackupEntry> entries;
    QVERIFY2(BackupManifest::load(manifest, &entries, &error), qPrintable(error));
    QCOMPARE(entries.size(), 1);
    QVERIFY2(engine.restoreFile(entries.first(), destination.path(), provider, &error), qPrintable(error));
    QFile restored(destination.filePath("retry.txt"));
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), QByteArray("new!"));
    QCOMPARE(provider.uploadedPayloads.size(), 2);
    for (const QString &path : provider.uploadedPayloads) {
        QVERIFY(!QFileInfo::exists(QFileInfo(path).path()));
    }
}

void BackupEngineTest::reservesManifestPathForSourceFiles()
{
    QTemporaryDir source;
    QTemporaryDir remote;
    QVERIFY(source.isValid());
    QVERIFY(remote.isValid());

    QFile original(source.filePath(QStringLiteral("manifest.json")));
    QVERIFY(original.open(QIODevice::WriteOnly));
    original.write("source manifest");
    original.close();

    BackupEngine engine;
    LocalProvider provider(remote.path());
    QString manifestPath;
    QString error;
    QVERIFY(engine.backup(source.path(), QStringLiteral("copy"), provider, &manifestPath, &error));

    QVector<BackupEntry> entries;
    QVERIFY(BackupManifest::load(manifestPath, &entries, &error));
    QCOMPARE(entries.size(), 1);
    QVERIFY(entries.first().remotePath != QStringLiteral("copy/manifest.json"));
    QVERIFY(QFileInfo::exists(remote.filePath(QStringLiteral("copy/manifest.json"))));

    QFile stored(remote.filePath(entries.first().remotePath));
    QVERIFY(stored.open(QIODevice::ReadOnly));
    QCOMPARE(stored.readAll(), QByteArray("source manifest"));
}

void BackupEngineTest::previewsMultipleSourcesAndExclusions()
{
    QTemporaryDir first;
    QTemporaryDir second;
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    QVERIFY(QDir().mkpath(first.filePath(QStringLiteral("cache"))));

    QFile included(first.filePath(QStringLiteral("keep.txt")));
    QVERIFY(included.open(QIODevice::WriteOnly));
    included.write("keep");
    included.close();
    QFile excluded(first.filePath(QStringLiteral("cache/drop.txt")));
    QVERIFY(excluded.open(QIODevice::WriteOnly));
    excluded.write("drop");
    excluded.close();
    QFile secondFile(second.filePath(QStringLiteral("same-name.txt")));
    QVERIFY(secondFile.open(QIODevice::WriteOnly));
    secondFile.write("second");
    secondFile.close();

    BackupEngine engine;
    const BackupPreview preview = engine.preview(
        {first.path(), second.path()},
        {first.filePath(QStringLiteral("cache"))}
    );

    QCOMPARE(preview.includedFiles.size(), 2);
    QCOMPARE(preview.excludedFiles, QStringList {excluded.fileName()});
    QVERIFY(preview.includedFiles.contains(included.fileName()));
    QVERIFY(preview.includedFiles.contains(secondFile.fileName()));
}

void BackupEngineTest::excludesMatchingFolderNamesAtEveryDepth_data()
{
    QTest::addColumn<QString>("rule");
    QTest::newRow("folder name") << QStringLiteral("node_modules");
    QTest::newRow("trailing slash") << QStringLiteral("node_modules/");
}

void BackupEngineTest::cancelledPreviewStopsTraversalAndDiscardsPartialResults()
{
    QTemporaryDir source;
    for (int i = 0; i < 100; ++i) {
        QFile file(source.filePath(QString("file-%1").arg(i)));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("payload");
    }
    BackupEngine engine;
    int checks = 0;
    const auto cancelled = engine.preview({source.path()}, {}, [&] { return ++checks >= 10; });
    QVERIFY(checks >= 10 && checks < 100);
    QVERIFY(cancelled.includedFiles.isEmpty());
    QVERIFY(cancelled.excludedFiles.isEmpty());
    QVERIFY(cancelled.skippedPaths.isEmpty());
    QVERIFY(cancelled.missingPaths.isEmpty());
    QCOMPARE(engine.preview({source.path()}, {}).includedFiles.size(), 100);
}

void BackupEngineTest::excludesMatchingFolderNamesAtEveryDepth()
{
    QFETCH(QString, rule);
    QTemporaryDir source;
    QTemporaryDir remote;
    QVERIFY(source.isValid());
    QVERIFY(remote.isValid());
    const QStringList included {QStringLiteral("src/app.js"), QStringLiteral("node_modules-old/keep.js"), QStringLiteral("notes/node_modules")};
    const QStringList excluded {QStringLiteral("node_modules/package/index.js"), QStringLiteral("projects/app/node_modules/dependency/index.js")};
    for (const QString &relative : included + excluded) {
        QVERIFY(QDir().mkpath(QFileInfo(source.filePath(relative)).path()));
        QFile file(source.filePath(relative));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("content");
    }
    const QString link = source.filePath(QStringLiteral("node_modules/linked-package"));
    QVERIFY(QFile::link(source.filePath(QStringLiteral("src/app.js")), link));

    BackupEngine engine;
    const BackupPreview preview = engine.preview({source.path()}, {rule});
    QCOMPARE(preview.includedFiles.size(), included.size());
    QCOMPARE(preview.excludedFiles.size(), excluded.size() + 1);
    QVERIFY(preview.excludedFiles.contains(link));
    QVERIFY(preview.skippedPaths.isEmpty());
    for (const QString &relative : included) QVERIFY(preview.includedFiles.contains(source.filePath(relative)));
    for (const QString &relative : excluded) QVERIFY(preview.excludedFiles.contains(source.filePath(relative)));

    LocalProvider provider(remote.path());
    QString manifestPath;
    QString error;
    QVERIFY2(engine.backup({source.path()}, QStringLiteral("copy"), {rule}, provider, &manifestPath, &error), qPrintable(error));
    QVector<BackupEntry> entries;
    QVERIFY(BackupManifest::load(manifestPath, &entries, &error));
    QCOMPARE(entries.size(), included.size());
    QVERIFY(!QFileInfo::exists(remote.filePath(QStringLiteral("copy/node_modules"))));
    QVERIFY(!QFileInfo::exists(remote.filePath(QStringLiteral("copy/projects/app/node_modules"))));
}

void BackupEngineTest::absoluteExclusionDoesNotExcludeSameNamedFoldersElsewhere()
{
    QTemporaryDir source;
    QVERIFY(source.isValid());
    for (const QString &project : {QStringLiteral("one"), QStringLiteral("two")}) {
        const QString folder = source.filePath(project + QStringLiteral("/node_modules"));
        QVERIFY(QDir().mkpath(folder));
        QFile file(QDir(folder).filePath(QStringLiteral("index.js")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("content");
    }
    BackupEngine engine;
    const BackupPreview preview = engine.preview({source.path()}, {source.filePath(QStringLiteral("one/node_modules"))});
    QCOMPARE(preview.includedFiles, QStringList {source.filePath(QStringLiteral("two/node_modules/index.js"))});
    QCOMPARE(preview.excludedFiles, QStringList {source.filePath(QStringLiteral("one/node_modules/index.js"))});
}

void BackupEngineTest::backsUpMultipleSourcesWithoutCollisions()
{
    QTemporaryDir first;
    QTemporaryDir second;
    QTemporaryDir remote;
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    QVERIFY(remote.isValid());

    QFile firstFile(first.filePath(QStringLiteral("same.txt")));
    QVERIFY(firstFile.open(QIODevice::WriteOnly));
    firstFile.write("first");
    firstFile.close();
    QFile secondFile(second.filePath(QStringLiteral("same.txt")));
    QVERIFY(secondFile.open(QIODevice::WriteOnly));
    secondFile.write("second");
    secondFile.close();

    BackupEngine engine;
    LocalProvider provider(remote.path());
    QString manifestPath;
    QString error;
    QVERIFY(engine.backup(
        {first.path(), second.path()}, QStringLiteral("copy"), {}, provider, &manifestPath, &error
    ));

    QVector<BackupEntry> entries;
    QVERIFY(BackupManifest::load(manifestPath, &entries, &error));
    QCOMPARE(entries.size(), 2);
    QVERIFY(entries.at(0).remotePath != entries.at(1).remotePath);
    QVERIFY(QFileInfo::exists(remote.filePath(entries.at(0).remotePath)));
    QVERIFY(QFileInfo::exists(remote.filePath(entries.at(1).remotePath)));
}

void BackupEngineTest::preservesVerifiedItemsInAnIncompleteCopy()
{
    QTemporaryDir source;
    QTemporaryDir remote;
    QVERIFY(source.isValid());
    QVERIFY(remote.isValid());

    QFile file(source.filePath(QStringLiteral("available.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("available");
    file.close();

    BackupEngine engine;
    LocalProvider provider(remote.path());
    QString manifestPath;
    QString error;
    QVERIFY(!engine.backup(
        {source.path(), source.filePath(QStringLiteral("missing"))},
        QStringLiteral("copy"), {}, provider, &manifestPath, &error
    ));
    QVERIFY(error.startsWith(QStringLiteral("Backup incomplete:")));
    QVERIFY(QFileInfo::exists(manifestPath));

    QVector<BackupEntry> entries;
    QVERIFY(BackupManifest::load(manifestPath, &entries, &error));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().sourcePath, file.fileName());
}

void BackupEngineTest::retriesOnlyReuseMatchingChecksummedPayloads_data()
{
    QTest::addColumn<bool>("omitChecksum");
    QTest::addColumn<QByteArray>("retryContent");
    for (bool omitChecksum : {false, true}) {
        const QString suffix = omitChecksum ? " size only" : " checksummed";
        QTest::newRow(qPrintable("unchanged" + suffix)) << omitChecksum << QByteArray("retry content");
        QTest::newRow(qPrintable("same-sized change" + suffix)) << omitChecksum << QByteArray("other content");
        QTest::newRow(qPrintable("size change" + suffix)) << omitChecksum << QByteArray("short");
    }
}

void BackupEngineTest::retriesOnlyReuseMatchingChecksummedPayloads()
{
    QFETCH(bool, omitChecksum);
    QFETCH(QByteArray, retryContent);
    QTemporaryDir source;
    QTemporaryDir remote;
    QVERIFY(source.isValid());
    QVERIFY(remote.isValid());

    QFile file(source.filePath(QStringLiteral("retry.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("retry content");
    file.close();

    BackupEngine engine;
    FailingProvider provider(remote.path());
    provider.omitChecksum = omitChecksum;
    QString manifestPath;
    QString error;
    QVERIFY(engine.backup(source.path(), QStringLiteral("copy"), provider, &manifestPath, &error));
    QDir(QFileInfo(manifestPath).path()).removeRecursively();
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(retryContent), qint64(retryContent.size()));
    file.close();
    QVERIFY(engine.backup(source.path(), QStringLiteral("copy"), provider, &manifestPath, &error));
    const auto cleanup = qScopeGuard([&] { QDir(QFileInfo(manifestPath).path()).removeRecursively(); });
    const bool reusable = !omitChecksum && retryContent == "retry content";
    QCOMPARE(provider.uploadedPayloads.size(), reusable ? 1 : 2);
    QFile uploaded(remote.filePath("copy/retry.txt"));
    QVERIFY(uploaded.open(QIODevice::ReadOnly));
    QCOMPARE(uploaded.readAll(), retryContent);
}

void BackupEngineTest::verifiesTransferredPayloadMetadata_data()
{
    QTest::addColumn<bool>("freshCopy");
    QTest::addColumn<bool>("omitChecksum");
    QTest::addColumn<QString>("damage");
    for (bool fresh : {false, true}) {
        const QString suffix = fresh ? " deferred" : " immediate";
        QTest::newRow(qPrintable("matching checksum" + suffix)) << fresh << false << QString();
        QTest::newRow(qPrintable("size only" + suffix)) << fresh << true << QString();
        QTest::newRow(qPrintable("size mismatch without checksum" + suffix)) << fresh << true << QString("truncated");
        QTest::newRow(qPrintable("size mismatch with checksum" + suffix)) << fresh << false << QString("truncated");
        QTest::newRow(qPrintable("checksum mismatch" + suffix)) << fresh << false << QString("corrupted");
    }
}

void BackupEngineTest::verifiesTransferredPayloadMetadata()
{
    QFETCH(bool, freshCopy);
    QFETCH(bool, omitChecksum);
    QFETCH(QString, damage);
    QTemporaryDir source, remote;
    QFile file(source.filePath("notes.txt"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("payload"), qint64(7));
    file.close();
    BackupEngine engine;
    FailingProvider provider(remote.path());
    provider.omitChecksum = omitChecksum;
    const BackupCopyMetadata metadata {"computer", "documents", "Documents", "copy", QDateTime::currentDateTimeUtc()};
    QString manifest, error;
    BackupResult result;
    QVector<BackupProgress> samples;
    const bool success = damage.isEmpty();
    QCOMPARE(engine.backup({source.path()}, "copy", {}, metadata, provider, &manifest, &error,
        [&](const BackupProgress &progress) {
            samples.append(progress);
            if (progress.phase != "verifying" || damage.isEmpty()) return;
            QFile uploaded(remote.filePath("copy/notes.txt"));
            QVERIFY(uploaded.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
            QVERIFY(uploaded.open(QIODevice::WriteOnly));
            const QByteArray contents = damage == "truncated" ? QByteArray("x") : QByteArray("corrupt");
            QCOMPARE(uploaded.write(contents), qint64(contents.size()));
        }, &result, {freshCopy}), success);
    const auto cleanup = qScopeGuard([&] { if (!manifest.isEmpty()) QDir(QFileInfo(manifest).path()).removeRecursively(); });
    QVERIFY2(result.manifestVerified, qPrintable(error));
    QCOMPARE(result.verifiedFiles, success ? 1 : 0);
    QCOMPARE(result.verifiedBytes, qint64(success ? 7 : 0));
    QCOMPARE(result.issues.size(), success ? 0 : 1);
    QCOMPARE(samples.last().verifiedFiles, result.verifiedFiles);
    QCOMPARE(samples.last().failedItems, result.issues.size());
    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    QVERIFY2(BackupManifest::load(manifest, &entries, &info, &error), qPrintable(error));
    QCOMPARE(entries.size(), result.verifiedFiles);
    QCOMPARE(info.status, success ? QString("complete") : QString("incomplete"));
    if (!success) {
        QCOMPARE(result.issues.first().phase, QString("verifying"));
        // Immediate verification retains the provider's earlier reuse-lookup
        // diagnostic; deferred verification starts with a fresh error string.
        QCOMPARE(result.issues.first().reason, freshCopy ? QString("Remote verification failed.")
            : QString("The remote file is unavailable."));
        QCOMPARE(info.failedItems, QStringList {"notes.txt"});
        QVERIFY(error.startsWith("Backup failed:"));
    }
}

void BackupEngineTest::backsUpAndRestoresHiddenContents_data()
{
    QTest::addColumn<QString>("rootName");
    QTest::newRow("visible root") << QStringLiteral("project");
    QTest::newRow("hidden root") << QStringLiteral(".project");
}

void BackupEngineTest::backsUpAndRestoresHiddenContents()
{
    QFETCH(QString, rootName);
    QTemporaryDir source;
    QTemporaryDir remote;
    QTemporaryDir destination;
    QVERIFY(source.isValid() && remote.isValid() && destination.isValid());
    const QDir root(source.filePath(rootName));
    const QStringList payloads {"visible.txt", ".secret", ".config/settings", ".config/nested/.token", "nested/.env"};
    QStringList expected;
    for (const QString &relative : payloads) {
        const QString path = root.filePath(relative);
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray content = "content for " + relative.toUtf8();
        QCOMPARE(file.write(content), qint64(content.size()));
        expected.append(path);
    }
    expected.sort();

    BackupEngine engine;
    const BackupPreview preview = engine.preview({root.path()}, {});
    QCOMPARE(preview.includedFiles, expected);
    QVERIFY(preview.excludedFiles.isEmpty());
    QVERIFY(preview.skippedPaths.isEmpty());
    QVERIFY(preview.missingPaths.isEmpty());
    // Directly selected hidden roots and files agree with folder traversal.
    const QStringList hiddenEntries {root.filePath(".config/nested/.token"), root.filePath(".config/settings"),
        root.filePath(".secret")};
    QCOMPARE(engine.preview({root.filePath(".secret"), root.filePath(".config")}, {}).includedFiles, hiddenEntries);

    LocalProvider provider(remote.path());
    QString manifest;
    QString error;
    BackupResult result;
    const BackupCopyMetadata metadata {"computer", "documents", "Documents", "copy", QDateTime::currentDateTimeUtc()};
    QVERIFY2(engine.backup({root.path()}, "computer/Documents/copy", {}, metadata,
        provider, &manifest, &error, {}, &result), qPrintable(error));
    QVERIFY(result.manifestVerified);
    QCOMPARE(result.verifiedFiles, payloads.size());
    QVERIFY(result.issues.isEmpty());
    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    QVERIFY2(BackupManifest::load(manifest, &entries, &info, &error), qPrintable(error));
    QCOMPARE(info.status, QString("complete"));
    QCOMPARE(entries.size(), payloads.size());
    QVERIFY(QDir(root.path()).removeRecursively());
    QStringList restoredPaths;
    for (const BackupEntry &entry : entries) {
        QVERIFY2(engine.restoreFile(entry, destination.path(), provider, &error), qPrintable(error));
        QFile restored(destination.filePath(entry.restorePath));
        QVERIFY(restored.open(QIODevice::ReadOnly));
        const QByteArray content = "content for " + entry.restorePath.toUtf8();
        QCOMPARE(restored.readAll(), content);
        QCOMPARE(entry.checksum, QCryptographicHash::hash(content, QCryptographicHash::Sha256));
        restoredPaths.append(entry.restorePath);
    }
    QStringList sortedPayloads = payloads;
    sortedPayloads.sort();
    restoredPaths.sort();
    QCOMPARE(restoredPaths, sortedPayloads);
}

void BackupEngineTest::appliesExclusionsToHiddenContents()
{
    QTemporaryDir source;
    QTemporaryDir remote;
    QVERIFY(source.isValid() && remote.isValid());
    const QStringList included {".env", ".config/settings", "other/.config/settings", ".git-old/keep"};
    const QStringList excluded {".git/config", "nested/.git/objects/data", ".config/private/.env",
        ".config/private/settings", "nested/.secret"};
    QStringList expectedIncluded;
    QStringList expectedExcluded;
    for (const QString &relative : included + excluded) {
        const QString path = source.filePath(relative);
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("content");
        (included.contains(relative) ? expectedIncluded : expectedExcluded).append(path);
    }
    expectedIncluded.sort();
    expectedExcluded.sort();
    const QStringList rules {".git", source.filePath(".config/private"), source.filePath("nested/.secret")};
    BackupEngine engine;
    const BackupPreview preview = engine.preview({source.path()}, rules);
    QCOMPARE(preview.includedFiles, expectedIncluded);
    QCOMPARE(preview.excludedFiles, expectedExcluded);
    QVERIFY(preview.skippedPaths.isEmpty());
    QCOMPARE(engine.preview({source.filePath(".git")}, rules).excludedFiles, QStringList {source.filePath(".git")});
    QCOMPARE(engine.preview({source.filePath("nested/.secret")}, rules).excludedFiles,
        QStringList {source.filePath("nested/.secret")});

    LocalProvider provider(remote.path());
    QString manifest;
    QString error;
    QVERIFY2(engine.backup({source.path()}, "copy", rules, provider, &manifest, &error), qPrintable(error));
    QVector<BackupEntry> entries;
    QVERIFY2(BackupManifest::load(manifest, &entries, &error), qPrintable(error));
    QStringList backedUp;
    for (const BackupEntry &entry : entries) backedUp.append(entry.sourcePath);
    backedUp.sort();
    QCOMPARE(backedUp, expectedIncluded);
    for (const QString &relative : excluded) QVERIFY(!QFileInfo::exists(remote.filePath("copy/" + relative)));
}

void BackupEngineTest::skipsHiddenSymbolicLinksUnlessExcluded()
{
    QTemporaryDir source;
    QTemporaryDir outside;
    QTemporaryDir remote;
    QVERIFY(source.isValid() && outside.isValid() && remote.isValid());
    for (const QString &path : {source.filePath(".env"), outside.filePath("outside.txt")}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("content");
    }
    const QString fileLink = source.filePath(".linked-file");
    const QString folderLink = source.filePath(".linked-folder");
    QVERIFY(QFile::link(outside.filePath("outside.txt"), fileLink));
    QVERIFY(QFile::link(outside.path(), folderLink));
    const QStringList links {fileLink, folderLink};
    const QStringList rules {fileLink, ".linked-folder"};
    BackupEngine engine;
    LocalProvider provider(remote.path());
    for (const bool excludeLinks : {false, true}) {
        const QStringList exclusions = excludeLinks ? rules : QStringList {};
        const BackupPreview preview = engine.preview({source.path()}, exclusions);
        QCOMPARE(preview.includedFiles, QStringList {source.filePath(".env")});
        QCOMPARE(preview.skippedPaths, excludeLinks ? QStringList {} : links);
        QCOMPARE(preview.excludedFiles, excludeLinks ? links : QStringList {});
        QString manifest;
        QString error;
        BackupResult result;
        const QString copy = excludeLinks ? "excluded" : "skipped";
        const BackupCopyMetadata metadata {"computer", "documents", "Documents", copy, QDateTime::currentDateTimeUtc()};
        QCOMPARE(engine.backup({source.path()}, "computer/Documents/" + copy, exclusions, metadata,
            provider, &manifest, &error, {}, &result), excludeLinks);
        QVERIFY(result.manifestVerified);
        QCOMPARE(result.verifiedFiles, 1);
        QCOMPARE(result.issues.size(), excludeLinks ? 0 : 2);
        for (const BackupIssue &issue : result.issues) {
            QCOMPARE(issue.phase, QString("selection"));
            QCOMPARE(issue.reason, QString("Symbolic links are not backed up."));
        }
        QVERIFY(!QFileInfo::exists(remote.filePath("computer/Documents/" + copy + "/.linked-file")));
        QVERIFY(!QFileInfo::exists(remote.filePath("computer/Documents/" + copy + "/.linked-folder")));
    }
}

void BackupEngineTest::listsRegularFilesAndSkipsSymlinks()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QFile keep(directory.filePath(QStringLiteral("keep.txt")));
    QVERIFY(keep.open(QIODevice::WriteOnly));
    keep.write("content");
    keep.close();

    QVERIFY(QDir().mkdir(directory.filePath(QStringLiteral("nested"))));
    QFile nested(directory.filePath(QStringLiteral("nested/zero-byte")));
    QVERIFY(nested.open(QIODevice::WriteOnly));
    nested.close();

    QVERIFY(QFile::link(keep.fileName(), directory.filePath(QStringLiteral("link.txt"))));

    BackupEngine engine;
    const QStringList files = engine.selectableFiles(directory.path());

    const QStringList expected {
        QFileInfo(keep).absoluteFilePath(),
        QFileInfo(nested).absoluteFilePath(),
    };

    QCOMPARE(files, expected);
}

void BackupEngineTest::localProviderRejectsUnsafePaths()
{
    QTemporaryDir remote;
    QVERIFY(remote.isValid());
    LocalProvider provider(remote.path());
    QString error;

    QVERIFY(!provider.upload(QStringLiteral("/tmp/file"), QStringLiteral("../outside"), &error));
    QCOMPARE(error, QStringLiteral("The provider path is invalid."));

    error.clear();
    QVERIFY(!provider.inspect(QStringLiteral("/absolute/file"), nullptr, &error));
    QCOMPARE(error, QStringLiteral("The provider path is invalid."));

    QTemporaryDir outside;
    QVERIFY(outside.isValid());
    QFile outsideFile(outside.filePath(QStringLiteral("outside.txt")));
    QVERIFY(outsideFile.open(QIODevice::WriteOnly));
    outsideFile.write("outside");
    outsideFile.close();
    QVERIFY(QFile::link(outsideFile.fileName(), remote.filePath(QStringLiteral("link.txt"))));
    QVERIFY(!provider.inspect(QStringLiteral("link.txt"), nullptr, &error));
    QCOMPARE(error, QStringLiteral("The provider path is invalid."));
    QVERIFY(QFile::link(outside.path(), remote.filePath(QStringLiteral("link-dir"))));
    QVERIFY(!provider.inspect(QStringLiteral("link-dir/../outside.txt"), nullptr, &error));
    QCOMPARE(error, QStringLiteral("The provider path is invalid."));
    QVERIFY(!provider.ensureDirectory(QStringLiteral("link-dir/new"), &error));
    QCOMPARE(error, QStringLiteral("The provider path is invalid."));
    QVERIFY(!provider.ensureDirectory(QStringLiteral("link-dir/new/deep"), &error));
    QCOMPARE(error, QStringLiteral("The provider path is invalid."));
}

QTEST_MAIN(BackupEngineTest)
#include "backupengine_test.moc"

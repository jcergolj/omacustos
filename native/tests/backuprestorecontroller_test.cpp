#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QScopeGuard>
#include <QTimer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QScopedPointer>

#include "../src/backuprestorecontroller.h"
#include "../src/backupmanifest.h"
#include "../src/backupecleanup.h"
#include "../src/localprovider.h"

class RestoreTestProvider final : public BackupProvider
{
public:
    explicit RestoreTestProvider(const QString &root) : local(root) {}
    LocalProvider local;
    QStringList listedPaths;
    QStringList downloadedPaths;
    QStringList inspectedPaths;
    QSemaphore entered;
    QSemaphore release;
    bool blockList = false;
    bool blockInspect = false;
    bool blockDownload = false;
    bool injectInvalidFolders = true;
    QString failedUploadPath;
    bool leaveFailedPayload = false;
    QStringList failedUploads;

    bool upload(const QString &source, const QString &path, QString *error) override
    {
        if (path == failedUploadPath) {
            failedUploads.append(path);
            if (leaveFailedPayload && !local.upload(source, path, error)) {
                return false;
            }
            if (error != nullptr) {
                *error = QStringLiteral("Upload failed.");
            }
            return false;
        }
        return local.upload(source, path, error);
    }
    bool ensureDirectory(const QString &path, QString *error) override { return local.ensureDirectory(path, error); }
    bool trash(const QString &path, QString *error) override { return local.trash(path, error); }
    bool permanentlyDelete(const QString &path, QString *error) override { return local.permanentlyDelete(path, error); }
    bool download(const QString &path, const QString &destination, QString *error) override
    {
        downloadedPaths.append(path);
        if (blockDownload) {
            entered.release();
            release.tryAcquire(1, 5000);
        }
        return local.download(path, destination, error);
    }
    bool inspect(const QString &path, RemoteFile *file, QString *error) override
    {
        inspectedPaths.append(path);
        if (blockInspect) {
            entered.release();
            release.tryAcquire(1, 5000);
        }
        return local.inspect(path, file, error);
    }
    bool list(const QString &path, QVector<RemoteItem> *items, QString *error) override
    {
        listedPaths.append(path);
        if (blockList) {
            entered.release();
            release.tryAcquire(1, 5000);
        }
        if (!local.list(path, items, error)) {
            return false;
        }
        if (injectInvalidFolders) {
            // A provider response must not expand discovery beyond direct child folders.
            items->append({"backups/other-computer/Other/copy", "copy", true, 0, {}});
            items->append({path + "/copy/nested", "nested", true, 0, {}});
        }
        return true;
    }
};

static bool createCopy(const QTemporaryDir &remote, const QString &folder, const QString &setId, const QString &copyId, int fileCount = 1, int version = 2)
{
    if (!QDir().mkpath(remote.filePath(folder + "/nested"))) {
        return false;
    }
    QJsonArray entries;
    QJsonArray expected;
    for (int i = 0; i < fileCount; ++i) {
        const QString name = fileCount == 1 ? QString("notes.txt") : QString("notes-%1.txt").arg(i);
        const QString relative = "nested/" + name;
        QFile file(remote.filePath(folder + "/" + relative));
        if (!file.open(QIODevice::WriteOnly) || file.write("notes") != 5) return false;
        entries.append(QJsonObject {{"source", "/source/" + name}, {"remote", folder + "/" + relative},
            {"restore", relative}, {"size", 5},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash("notes", QCryptographicHash::Sha256).toHex())}});
        expected.append(relative);
    }
    QFile manifest(remote.filePath(folder + "/manifest.json"));
    return manifest.open(QIODevice::WriteOnly)
        && manifest.write(QJsonDocument(QJsonObject {{"version", version}, {"application", "omacustos"},
            {"computer", "computer"}, {"set_id", setId}, {"set_name", "Documents"}, {"copy_id", copyId},
            {"created_at", "2026-10-02T12:00:00.000Z"}, {"status", "complete"},
            {"expected", expected}, {"failed", QJsonArray {}}, {"entries", entries}}).toJson()) > 0;
}

class BackupRestoreControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void restoresSurvivingFileAfterUploadFailure_data();
    void restoresSurvivingFileAfterUploadFailure();
    void loadsAndRestoresSelectedEntry();
    void rejectsInvalidSelection();
    void clearsEntriesWhenManifestFailsToLoad();
    void rejectsEmptyDestination();
    void completesOnlyAfterAllSelectedFilesAreRestored_data();
    void completesOnlyAfterAllSelectedFilesAreRestored();
    void restoresLegacySelections_data();
    void restoresLegacySelections();
    void boundsProgressNotificationsForLargeSelections();
    void restoresWithoutBlockingTheControllerThread();
    void navigationSupersedesDiscoveryWithoutPublishingOldResults();
    void navigationDuringRestoreKeepsContextLocked();
    void pauseResumeAndStopPreserveCommittedFiles_data();
    void pauseResumeAndStopPreserveCommittedFiles();
    void destructionStopsPausedRestore();
    void archivePauseRetainsDownloadAndReportsIssues_data();
    void archivePauseRetainsDownloadAndReportsIssues();
    void stopDuringDownloadDoesNotPublishDestination();
    void supersededVerificationCannotReplaceCurrentCopy();
    void supersededVerificationStopsBeforeInspectingRemainingFiles();
    void qmlEligibilityTracksVerificationAndRestore_data();
    void qmlEligibilityTracksVerificationAndRestore();
    void rediscoveryKeepsFilesCachedUntilReverification_data();
    void rediscoveryKeepsFilesCachedUntilReverification();
    void rediscoveryHandlesMissingCopies_data();
    void rediscoveryHandlesMissingCopies();
    void browsesWithoutBlockingAndVerifiesOnlyTheSelectedCopy();
    void discoveryFailureClearsPreviousResultsAndAllowsRetry();
    void rejectsUnidentifiableCopies_data();
    void rejectsUnidentifiableCopies();
};

void BackupRestoreControllerTest::supersededVerificationStopsBeforeInspectingRemainingFiles()
{
    QTemporaryDir remote;
    const QString folder = "backups/computer/Documents";
    QVERIFY(createCopy(remote, folder + "/copy-z", "documents", "copy-z", 100));
    QVERIFY(createCopy(remote, folder + "/copy-a", "documents", "copy-a"));
    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    BackupRestoreController controller(engine, &provider);
    const auto unblock = qScopeGuard([&] { provider.release.release(200); });
    controller.discover(folder, "documents");
    QTRY_VERIFY(!controller.busy());
    provider.blockInspect = true;
    controller.selectCopy(0);
    QTRY_VERIFY(provider.entered.available() > 0);
    provider.entered.acquire();
    controller.selectCopy(1);
    provider.release.release(200);
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(provider.inspectedPaths.size(), 2);
    QVERIFY(provider.inspectedPaths.first().startsWith(folder + "/copy-z/"));
    QCOMPARE(provider.inspectedPaths.last(), folder + "/copy-a/nested/notes.txt");
    QCOMPARE(controller.entries(), QStringList {"/source/notes.txt"});
    QVERIFY(controller.restoreEligible());
}

void BackupRestoreControllerTest::supersededVerificationCannotReplaceCurrentCopy()
{
    QTemporaryDir remote;
    const QString folder = "backups/computer/Documents";
    QVERIFY(createCopy(remote, folder + "/copy-a", "documents", "copy-a"));
    QVERIFY(createCopy(remote, folder + "/copy-b", "documents", "copy-b"));
    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    BackupRestoreController controller(engine, &provider);
    const auto unblock = qScopeGuard([&] { provider.release.release(10); });
    controller.discover(folder, "documents");
    QTRY_VERIFY(!controller.busy());
    provider.blockInspect = true;
    controller.selectCopy(0);
    QTRY_VERIFY(provider.entered.available() > 0);
    provider.entered.acquire();
    controller.selectCopy(1);
    QVERIFY(!controller.restoreEligible());
    provider.release.release(10);
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.currentCopyIndex(), 1);
    QCOMPARE(controller.currentCopyPath(), folder + "/copy-a");
    QVERIFY(controller.restoreEligible());
    QCOMPARE(provider.downloadedPaths, (QStringList {folder + "/copy-b/manifest.json", folder + "/copy-a/manifest.json"}));
}

void BackupRestoreControllerTest::navigationSupersedesDiscoveryWithoutPublishingOldResults()
{
    QTemporaryDir remote;
    const QString first = "backups/computer/Documents";
    const QString second = "backups/computer/Photos";
    QVERIFY(createCopy(remote, first + "/old-copy", "documents", "old-copy"));
    QVERIFY(createCopy(remote, second + "/new-copy", "photos", "new-copy"));
    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    provider.blockList = true;
    BackupRestoreController controller(engine, &provider);
    const auto unblock = qScopeGuard([&] { provider.release.release(10); });
    controller.discover(first, "documents");
    QTRY_VERIFY(provider.entered.available() > 0);
    provider.entered.acquire();
    controller.discover(second, "photos");
    provider.release.release(10);
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.copies().size(), 1);
    QVERIFY(controller.copies().first().contains("new-copy"));
    QVERIFY(!controller.restoreEligible());
    controller.selectCopy(0);
    QTRY_VERIFY(!controller.busy());
    QVERIFY(controller.restoreEligible());
    controller.discover(first, "documents");
    QTRY_VERIFY(!controller.busy());
    controller.discover(second, "photos");
    // Cached snapshot is available synchronously when returning, but never grants
    // restore eligibility before current-context verification.
    QCOMPARE(controller.entries(), QStringList {"/source/notes.txt"});
    QVERIFY(controller.showingCachedData());
    QVERIFY(!controller.restoreEligible());
    provider.release.release(10);
    QTRY_VERIFY(!controller.busy());
    QVERIFY(!controller.restoreEligible());
}

void BackupRestoreControllerTest::navigationDuringRestoreKeepsContextLocked()
{
    QTemporaryDir remote, destination;
    const QString first = "backups/computer/Documents";
    const QString second = "backups/computer/Photos";
    QVERIFY(createCopy(remote, first + "/copy", "documents", "copy", 3));
    QVERIFY(createCopy(remote, second + "/copy", "photos", "copy"));
    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    BackupRestoreController controller(engine, &provider);
    const auto unblock = qScopeGuard([&] { provider.release.release(10); });
    controller.discover(first, "documents");
    QTRY_VERIFY(!controller.busy());
    controller.selectCopy(0);
    QTRY_VERIFY(!controller.busy());
    QSignalSpy completed(&controller, &BackupRestoreController::restoreCompletedForContext);
    provider.blockDownload = true;
    QVariantList selection {0, 1, 2};
    controller.restoreSelected(selection, destination.path());
    selection.clear();
    QTRY_VERIFY(provider.entered.available() > 0);
    provider.entered.acquire();
    QCOMPARE(controller.restoreProgressFraction(), 0.0);
    controller.discover(second, "photos");
    controller.selectCopy(-1);
    QCOMPARE(controller.backupFolder(), first);
    QCOMPARE(controller.entries().size(), 3);
    QVERIFY(controller.property("restoring").toBool());
    QVERIFY(!controller.property("browsing").toBool());
    bool heartbeat = false;
    QTimer::singleShot(0, &controller, [&] { heartbeat = true; });
    QTRY_VERIFY(heartbeat);
    provider.release.release();
    QTRY_VERIFY(provider.entered.available() > 0);
    provider.entered.acquire();
    QTRY_COMPARE(controller.restoreProgress(), QString("1 of 3 files restored"));
    QVERIFY(QFile::exists(destination.filePath("nested/notes-0.txt")));
    QVERIFY(!QFile::exists(destination.filePath("nested/notes-1.txt")));
    QCOMPARE(completed.count(), 0);
    provider.release.release(10);
    QTRY_VERIFY(!controller.busy());
    for (int index = 0; index < 3; ++index) {
        QFile file(destination.filePath(QString("nested/notes-%1.txt").arg(index)));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("notes"));
    }
    QCOMPARE(completed.count(), 1);
    QCOMPARE(completed.first(), (QVariantList {first, "documents", first + "/copy"}));
    QCOMPARE(provider.listedPaths.last(), first);
    QCOMPARE(controller.entries().size(), 3);
    QCOMPARE(controller.property("restoreProgress").toString(), QString("3 of 3 files restored"));
    QCOMPARE(controller.restoreProgressFraction(), 1.0);
}

void BackupRestoreControllerTest::pauseResumeAndStopPreserveCommittedFiles_data()
{
    QTest::addColumn<bool>("stop");
    QTest::newRow("resume") << false;
    QTest::newRow("stop-while-paused") << true;
}

void BackupRestoreControllerTest::pauseResumeAndStopPreserveCommittedFiles()
{
    QFETCH(bool, stop);
    QTemporaryDir remote, destination;
    const QString folder = "backups/computer/Documents";
    QVERIFY(createCopy(remote, folder + "/copy", "documents", "copy", 2));
    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    BackupRestoreController controller(engine, &provider);
    const auto unblock = qScopeGuard([&] { provider.release.release(10); });
    controller.discover(folder, "documents");
    QTRY_VERIFY(!controller.busy());
    controller.selectCopy(0);
    QTRY_VERIFY(!controller.busy());
    QVERIFY(QDir().mkpath(destination.filePath("nested")));
    QFile existing(destination.filePath("nested/notes-1.txt"));
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write("original"), 8);
    existing.close();
    provider.blockDownload = true;
    QSignalSpy completed(&controller, &BackupRestoreController::restoreCompleted);
    QSignalSpy failed(&controller, &BackupRestoreController::failed);
    controller.restoreSelected({0, 1}, destination.path());
    QCOMPARE(controller.restoreState(), QString("running"));
    QCOMPARE(controller.restoreDestination(), destination.path());
    QCOMPARE(controller.restoreBackupName(), QString("Documents"));
    QCOMPARE(controller.restoreDownloadCost().value("bytes").toLongLong(), 10);
    QTRY_VERIFY(provider.entered.available() > 0);
    provider.entered.acquire();
    provider.release.release();
    QTRY_VERIFY(provider.entered.available() > 0);
    provider.entered.acquire();
    QTRY_COMPARE(controller.restoreProgress(), QString("1 of 2 files restored"));
    controller.pauseRestore();
    QCOMPARE(controller.restoreState(), QString("pausing"));
    provider.release.release();
    QTRY_COMPARE(controller.restoreState(), QString("paused"));
    QVERIFY(controller.busy());
    QVERIFY(!controller.restoreEligible());
    QCOMPARE(completed.count(), 0);
    const int downloads = provider.downloadedPaths.size();
    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), QByteArray("original"));
    existing.close();
    if (stop) controller.stopRestore();
    else controller.resumeRestore();
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.restoreState(), stop ? QString("stopped") : QString("succeeded"));
    QCOMPARE(controller.restoreProgress(), stop ? QString("1 of 2 files restored") : QString("2 of 2 files restored"));
    QCOMPARE(completed.count(), stop ? 0 : 1);
    QCOMPARE(failed.count(), 0);
    QVERIFY(controller.restoreIssues().isEmpty());
    QCOMPARE(provider.downloadedPaths.size(), downloads); // Resume reuses the download.
    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), stop ? QByteArray("original") : QByteArray("notes"));
    existing.close();
    QVERIFY(QFile::exists(destination.filePath("nested/notes-0.txt")));
    QCOMPARE(QDir(destination.filePath("nested")).entryList({".omacustos-restore-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).size(), 0);
    QVERIFY(controller.restoreEligible());
}

void BackupRestoreControllerTest::destructionStopsPausedRestore()
{
    QTemporaryDir remote, destination;
    const QString folder = "backups/computer/Documents";
    QVERIFY(createCopy(remote, folder + "/copy", "documents", "copy"));
    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    auto controller = std::make_unique<BackupRestoreController>(engine, &provider);
    const auto unblock = qScopeGuard([&] { provider.release.release(10); });
    controller->discover(folder, "documents");
    QTRY_VERIFY(!controller->busy());
    controller->selectCopy(0);
    QTRY_VERIFY(!controller->busy());
    provider.blockDownload = true;
    controller->restoreSelected({0}, destination.path());
    QTRY_VERIFY(provider.entered.available() > 0);
    provider.entered.acquire();
    controller->pauseRestore();
    provider.release.release();
    QTRY_COMPARE(controller->restoreState(), QString("paused"));
    QElapsedTimer timer;
    timer.start();
    controller.reset();
    QVERIFY(timer.elapsed() < 1000);
    QVERIFY(!QFile::exists(destination.filePath("nested/notes.txt")));
    QCOMPARE(QDir(destination.filePath("nested")).entryList({".omacustos-restore-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).size(), 0);
}

void BackupRestoreControllerTest::stopDuringDownloadDoesNotPublishDestination()
{
    QTemporaryDir remote, destination;
    const QString copy = "backups/computer/Documents/copy";
    QVERIFY(createCopy(remote, copy, "documents", "copy"));
    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    BackupRestoreController controller(engine, &provider);
    const auto unblock = qScopeGuard([&] { provider.release.release(10); });
    controller.loadManifest(remote.filePath(copy + "/manifest.json"));
    provider.blockDownload = true;
    QSignalSpy failed(&controller, &BackupRestoreController::failed);
    controller.restoreSelected({0}, destination.path());
    QTRY_VERIFY(provider.entered.available() > 0);
    provider.entered.acquire();
    controller.stopRestore();
    QCOMPARE(controller.restoreState(), QString("stopping"));
    QVERIFY(controller.stopRequested());
    QVERIFY(controller.isRestoring());
    provider.release.release();
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.restoreState(), QString("stopped"));
    QCOMPARE(controller.restoreProgress(), QString("0 of 1 files restored"));
    QCOMPARE(failed.count(), 0);
    QVERIFY(!controller.stopRequested());
    QVERIFY(!QFile::exists(destination.filePath("nested/notes.txt")));
    // A stopped token must not poison subsequent provider calls or retries.
    provider.blockDownload = false;
    controller.restoreSelected({0}, destination.path());
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.restoreState(), QString("succeeded"));
}

void BackupRestoreControllerTest::archivePauseRetainsDownloadAndReportsIssues_data()
{
    QTest::addColumn<QString>("action");
    QTest::newRow("resume-archive") << QString("resume");
    QTest::newRow("stop-archive") << QString("stop");
    QTest::newRow("corrupt-archive") << QString("corrupt");
}

void BackupRestoreControllerTest::archivePauseRetainsDownloadAndReportsIssues()
{
    QFETCH(QString, action);
    QTemporaryDir directory(QDir::current().filePath("restore-archive-controls-XXXXXX"));
    QVERIFY(directory.isValid());
    const QString source = directory.filePath("source");
    QVERIFY(QDir().mkpath(source));
    QFile file(QDir(source).filePath("notes.txt"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("notes"), 5);
    file.close();
    QVERIFY(QDir().mkpath(directory.filePath("remote")));
    RestoreTestProvider provider(directory.filePath("remote"));
    BackupEngine engine;
    BackupOptions options;
    options.freshCopy = true;
    options.boundedArchives = true;
    options.stagingDirectory = directory.filePath("stage");
    options.stagingBudget = 200000;
    const BackupCopyMetadata metadata {"computer", "documents", "Documents", "copy", QDateTime::currentDateTimeUtc()};
    QString manifest, error;
    QVERIFY2(engine.backup({source}, "copies/copy", {}, metadata, provider, &manifest, &error, {}, nullptr, options), qPrintable(error));
    QVector<BackupEntry> entries;
    QVERIFY2(BackupManifest::load(manifest, &entries, &error), qPrintable(error));
    QCOMPARE(entries.size(), 1);
    QVERIFY(!entries.first().archive.id.isEmpty());
    if (action == "corrupt") {
        QFile archive(directory.filePath("remote/" + entries.first().archive.remotePath));
        QVERIFY(archive.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(archive.write("corrupt"), 7);
    }
    BackupRestoreController controller(engine, &provider);
    const auto unblock = qScopeGuard([&] { provider.release.release(10); });
    controller.loadManifest(manifest);
    const QString destination = directory.filePath("destination");
    provider.blockDownload = true;
    controller.restoreSelected({0}, destination);
    QTRY_VERIFY(provider.entered.available() > 0);
    provider.entered.acquire();
    controller.pauseRestore();
    provider.release.release();
    QTRY_COMPARE(controller.restoreState(), QString("paused"));
    QCOMPARE(controller.restoreProgress(), QString("0 of 1 files restored"));
    QCOMPARE(provider.downloadedPaths.size(), 1);
    if (action == "stop") controller.stopRestore();
    else controller.resumeRestore();
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(provider.downloadedPaths.size(), 1);
    QCOMPARE(controller.restoreState(), action == "stop" ? QString("stopped")
        : action == "corrupt" ? QString("failed") : QString("succeeded"));
    QCOMPARE(QFile::exists(QDir(destination).filePath(entries.first().restorePath)), action == "resume");
    if (action == "corrupt") {
        QCOMPARE(controller.restoreIssues().size(), 1);
        const auto issue = controller.restoreIssues().first().toMap();
        QCOMPARE(issue.value("path").toString(), entries.first().archive.remotePath);
        QCOMPARE(issue.value("phase").toString(), QString("Verifying archive"));
    } else QVERIFY(controller.restoreIssues().isEmpty());
}

void BackupRestoreControllerTest::restoresSurvivingFileAfterUploadFailure_data()
{
    QTest::addColumn<bool>("leaveFailedPayload");
    QTest::newRow("failed upload leaves no file") << false;
    QTest::newRow("failed upload leaves remote payload") << true;
}

void BackupRestoreControllerTest::restoresSurvivingFileAfterUploadFailure()
{
    QFETCH(bool, leaveFailedPayload);
    QTemporaryDir source;
    QTemporaryDir remote;
    QTemporaryDir destination;
    QVERIFY(source.isValid());
    QVERIFY(remote.isValid());
    QVERIFY(destination.isValid());
    for (const QString &name : {QString("notes.txt"), QString("failed.txt")}) {
        QFile file(source.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("notes"), qint64(5));
    }

    const QString folder = "backups/computer/Documents";
    const QString copy = folder + "/copy";
    const BackupCopyMetadata metadata {"computer", "set-id", "Documents", "copy", QDateTime::currentDateTimeUtc()};
    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    provider.injectInvalidFolders = false;
    provider.failedUploadPath = copy + "/failed.txt";
    provider.leaveFailedPayload = leaveFailedPayload;
    QString manifestPath;
    QString error;
    BackupResult result;
    QVERIFY(!engine.backup({source.path()}, copy, {}, metadata, provider, &manifestPath, &error, {}, &result));
    const auto cleanupManifest = qScopeGuard([&] {
        if (!manifestPath.isEmpty()) {
            QDir(QFileInfo(manifestPath).path()).removeRecursively();
        }
    });
    QCOMPARE(provider.failedUploads, (QStringList {copy + "/failed.txt", copy + "/failed.txt"}));
    QVERIFY(result.manifestVerified);
    QCOMPARE(result.verifiedFiles, qint64(1));
    QCOMPARE(result.verifiedBytes, qint64(5));
    QCOMPARE(result.issues.size(), 1);
    QCOMPARE(result.issues.first().path, source.filePath("failed.txt"));
    QCOMPARE(result.issues.first().phase, QStringLiteral("uploading"));
    QCOMPARE(QFile::exists(remote.filePath(copy + "/failed.txt")), leaveFailedPayload);

    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    QVERIFY2(BackupManifest::load(manifestPath, &entries, &info, &error), qPrintable(error));
    QCOMPARE(info.version, 2);
    QCOMPARE(info.status, QStringLiteral("incomplete"));
    QVERIFY(info.expectedItems.contains("notes.txt"));
    QVERIFY(info.expectedItems.contains("failed.txt"));
    QCOMPARE(info.failedItems, QStringList {"failed.txt"});
    QCOMPARE(info.issues.size(), 1);
    QCOMPARE(info.issues.first().path, result.issues.first().path);
    QCOMPARE(info.issues.first().phase, result.issues.first().phase);
    QCOMPARE(info.issues.first().reason, result.issues.first().reason);
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().restorePath, QStringLiteral("notes.txt"));

    QVector<RemoteCopy> copies;
    error.clear();
    QVERIFY2(BackupCatalog::discover(provider, "backups", &copies, &error), qPrintable(error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(copies.size(), 1);
    QCOMPARE(copies.first().status, QStringLiteral("incomplete"));
    QCOMPARE(copies.first().entries.size(), 1);
    QCOMPARE(copies.first().entries.first().restorePath, QStringLiteral("notes.txt"));
    QCOMPARE(copies.first().failedItems, QStringList {"failed.txt"});
    QVERIFY(!copies.first().complete());
    QCOMPARE(BackupCleanup::eligibleTargets(copies, 1, "computer", "set-id"), QStringList {copy});

    BackupRestoreController controller(engine, &provider);
    QSignalSpy failed(&controller, &BackupRestoreController::failed);
    QSignalSpy completed(&controller, &BackupRestoreController::restoreCompleted);
    controller.discover(folder, "set-id");
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.copies().size(), 1);
    controller.selectCopy(0);
    QTRY_VERIFY(!controller.busy());
    QVERIFY(failed.isEmpty());
    QVERIFY(controller.restoreEligible());
    QCOMPARE(controller.entries(), QStringList {source.filePath("notes.txt")});
    QCOMPARE(controller.unavailableEntries(), QStringList {"failed.txt"});
    QVERIFY(controller.copies().first().contains("incomplete"));
    QVERIFY(controller.copies().first().contains("some items unavailable"));

    const int downloadsBeforeRestore = provider.downloadedPaths.size();
    controller.restore(1, destination.path());
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.first().first().toString(), QStringLiteral("The selected restore file is invalid."));
    QCOMPARE(provider.downloadedPaths.size(), downloadsBeforeRestore);
    QVERIFY(!QFile::exists(destination.filePath("failed.txt")));

    controller.restoreSelected({0}, destination.path());
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(completed.count(), 1);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(provider.downloadedPaths.size(), downloadsBeforeRestore + 1);
    QCOMPARE(provider.downloadedPaths.last(), copy + "/notes.txt");
    QFile restored(destination.filePath("notes.txt"));
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), QByteArray("notes"));
    QVERIFY(!QFile::exists(destination.filePath("failed.txt")));
}

void BackupRestoreControllerTest::qmlEligibilityTracksVerificationAndRestore_data()
{
    QTest::addColumn<bool>("singleFile");
    QTest::addColumn<bool>("failRestore");
    QTest::newRow("single file succeeds") << true << false;
    QTest::newRow("single file fails") << true << true;
    QTest::newRow("selected files succeed") << false << false;
    QTest::newRow("selected files fail") << false << true;
}

void BackupRestoreControllerTest::qmlEligibilityTracksVerificationAndRestore()
{
    QFETCH(bool, singleFile);
    QFETCH(bool, failRestore);
    QTemporaryDir remote;
    QTemporaryDir destination;
    QVERIFY(remote.isValid());
    QVERIFY(destination.isValid());
    const QString folder = "backups/computer/Documents";
    const QString copy = folder + "/copy";
    QVERIFY(createCopy(remote, copy, "set-id", "copy"));
    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    BackupRestoreController controller(engine, &provider);
    const auto unblock = qScopeGuard([&] { provider.release.release(10); });
    QSignalSpy completed(&controller, &BackupRestoreController::restoreCompleted);
    QSignalSpy failed(&controller, &BackupRestoreController::failed);

    QQmlEngine qmlEngine;
    qmlEngine.rootContext()->setContextProperty("restoreController", &controller);
    QQmlComponent component(&qmlEngine);
    component.setData(R"(import QtQml
        QtObject {
            property bool eligible: restoreController.restoreEligible
            property var selectedRestoreIndexes: []
            property string destination: ""
            property bool startEnabled: restoreController.restoreEligible
                && selectedRestoreIndexes.length > 0 && destination.trim().length > 0
        })", QUrl());
    QScopedPointer<QObject> view(component.create());
    QVERIFY2(view, qPrintable(component.errorString()));
    QVERIFY(!view->property("eligible").toBool());

    controller.discover(folder, "set-id");
    QTRY_VERIFY(!controller.busy());
    provider.blockInspect = true;
    controller.selectCopy(0);
    QTRY_VERIFY(provider.entered.available() > 0);
    provider.entered.acquire();
    QVERIFY(!view->property("eligible").toBool());
    provider.release.release();
    QTRY_VERIFY(!controller.busy());
    QVERIFY(view->property("eligible").toBool());
    QVERIFY(!view->property("startEnabled").toBool());
    QVERIFY(view->setProperty("selectedRestoreIndexes", QVariantList {0}));
    QVERIFY(!view->property("startEnabled").toBool());
    QVERIFY(view->setProperty("destination", destination.path()));
    QVERIFY(view->property("startEnabled").toBool());

    provider.blockDownload = true;
    if (failRestore) {
        QVERIFY(QFile::remove(remote.filePath(copy + "/nested/notes.txt")));
    }
    if (singleFile) {
        controller.restore(0, destination.path());
    } else {
        controller.restoreSelected({0}, destination.path());
    }
    QTRY_VERIFY(provider.entered.available() > 0);
    provider.entered.acquire();
    QVERIFY(controller.busy());
    QVERIFY(!view->property("eligible").toBool());
    QVERIFY(!view->property("startEnabled").toBool());
    // Editing a destination during transfer must not prevent retry after failure.
    QVERIFY(view->setProperty("destination", destination.filePath("retry")));
    provider.release.release();
    QTRY_VERIFY(!controller.busy());
    QVERIFY(view->property("eligible").toBool());
    QVERIFY(view->property("startEnabled").toBool());
    QCOMPARE(completed.count(), failRestore ? 0 : 1);
    QCOMPARE(failed.count(), failRestore ? 1 : 0);
    QCOMPARE(QFile::exists(destination.filePath("nested/notes.txt")), !failRestore);

    if (failRestore) {
        QVERIFY(createCopy(remote, copy, "set-id", "copy"));
        provider.blockDownload = false;
        controller.restoreSelected({0}, destination.filePath("retry"));
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(completed.count(), 1);
        QVERIFY(QFile::exists(destination.filePath("retry/nested/notes.txt")));
    }
}

void BackupRestoreControllerTest::rediscoveryKeepsFilesCachedUntilReverification_data()
{
    QTest::addColumn<bool>("removeFile");
    QTest::newRow("file still available") << false;
    QTest::newRow("file disappeared") << true;
}

void BackupRestoreControllerTest::rediscoveryKeepsFilesCachedUntilReverification()
{
    QFETCH(bool, removeFile);
    QTemporaryDir remote;
    QTemporaryDir destination;
    QVERIFY(remote.isValid());
    QVERIFY(destination.isValid());
    const QString folder = "backups/computer/Documents";
    const QString copy = folder + "/20261001-copy";
    QVERIFY(createCopy(remote, copy, "set-id", "20261001-copy"));
    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    BackupRestoreController controller(engine, &provider);
    controller.discover(folder, "set-id");
    QTRY_VERIFY(!controller.busy());
    controller.selectCopy(0);
    QTRY_VERIFY(!controller.busy());
    QVERIFY(controller.restoreEligible());
    QVERIFY(!controller.showingCachedData());

    // A new copy shifts the selected copy's index without changing its identity.
    QVERIFY(createCopy(remote, folder + "/20261002-copy", "set-id", "20261002-copy"));
    if (removeFile) {
        QVERIFY(QFile::remove(remote.filePath(copy + "/nested/notes.txt")));
    }
    controller.discover(folder, "set-id");
    QVERIFY(controller.busy());
    QVERIFY(controller.showingCachedData());
    QCOMPARE(controller.entries(), QStringList {"/source/notes.txt"});
    QVERIFY(!controller.restoreEligible());
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.currentCopyIndex(), 1);
    QCOMPARE(controller.entries(), QStringList {"/source/notes.txt"});
    QVERIFY(controller.showingCachedData());
    QVERIFY(!controller.restoreEligible());
    QCOMPARE(provider.downloadedPaths, QStringList {copy + "/manifest.json"});

    QSignalSpy failed(&controller, &BackupRestoreController::failed);
    controller.restoreSelected({0}, destination.path());
    QCOMPARE(failed.count(), 1);
    QVERIFY(failed.first().first().toString().contains("not currently verified"));
    QVERIFY(!QFile::exists(destination.filePath("nested/notes.txt")));

    controller.selectCopy(1);
    QVERIFY(controller.showingCachedData());
    QTRY_VERIFY(!controller.busy());
    QVERIFY(!controller.showingCachedData());
    QCOMPARE(controller.currentCopyIndex(), 1);
    QCOMPARE(provider.downloadedPaths.size(), 2);
    if (removeFile) {
        QVERIFY(controller.entries().isEmpty());
        QCOMPARE(controller.unavailableEntries(), QStringList {"nested/notes.txt"});
    } else {
        QCOMPARE(controller.entries(), QStringList {"/source/notes.txt"});
        QVERIFY(controller.restoreEligible());
        controller.restoreSelected({0}, destination.path());
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(failed.count(), 1);
        QVERIFY(QFile::exists(destination.filePath("nested/notes.txt")));
    }
}

void BackupRestoreControllerTest::rediscoveryHandlesMissingCopies_data()
{
    QTest::addColumn<bool>("failDiscovery");
    QTest::newRow("selected copy disappeared") << false;
    QTest::newRow("discovery failed") << true;
}

void BackupRestoreControllerTest::rediscoveryHandlesMissingCopies()
{
    QFETCH(bool, failDiscovery);
    QTemporaryDir remote;
    QVERIFY(remote.isValid());
    const QString folder = "backups/computer/Documents";
    const QString copy = folder + "/copy";
    QVERIFY(createCopy(remote, copy, "set-id", "copy"));
    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    BackupRestoreController controller(engine, &provider);
    controller.discover(folder, "set-id");
    QTRY_VERIFY(!controller.busy());
    controller.selectCopy(0);
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.entries(), QStringList {"/source/notes.txt"});

    QVERIFY(QDir(remote.filePath(failDiscovery ? folder : copy)).removeRecursively());
    QSignalSpy failed(&controller, &BackupRestoreController::failed);
    controller.discover(folder, "set-id");
    QTRY_VERIFY(!controller.busy());
    QVERIFY(!controller.restoreEligible());
    QCOMPARE(failed.count(), failDiscovery ? 1 : 0);
    if (failDiscovery) {
        QCOMPARE(controller.entries(), QStringList {"/source/notes.txt"});
        QCOMPARE(controller.copies().size(), 1);
        QCOMPARE(controller.currentCopyIndex(), 0);
        QVERIFY(controller.showingCachedData());
    } else {
        QVERIFY(controller.entries().isEmpty());
        QVERIFY(controller.copies().isEmpty());
        QCOMPARE(controller.currentCopyIndex(), -1);
        QVERIFY(!controller.showingCachedData());
    }

    QVERIFY(createCopy(remote, copy, "set-id", "copy"));
    controller.discover(folder, "set-id");
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.copies().size(), 1);
    QVERIFY(!controller.restoreEligible());
    controller.selectCopy(0);
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.entries(), QStringList {"/source/notes.txt"});
    QVERIFY(controller.restoreEligible());
    QVERIFY(!controller.showingCachedData());
}

void BackupRestoreControllerTest::browsesWithoutBlockingAndVerifiesOnlyTheSelectedCopy()
{
    QTemporaryDir remote;
    QVERIFY(remote.isValid());
    const QString folder = "backups/computer/Documents";
    const QString newest = folder + "/20261002-copy";
    QVERIFY(createCopy(remote, newest, "set-id", "20261002-copy"));
    QVERIFY(createCopy(remote, folder + "/20261001-copy", "set-id", "20261001-copy"));
    QVERIFY(createCopy(remote, "backups/other-computer/Other/copy", "other-id", "copy"));
    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    provider.blockList = true;
    BackupRestoreController controller(engine, &provider);
    const auto unblock = qScopeGuard([&] { provider.release.release(10); });
    QSignalSpy failures(&controller, &BackupRestoreController::failed);

    controller.discover(folder, "set-id");
    QVERIFY(controller.busy());
    QTRY_VERIFY(provider.entered.available() > 0);
    provider.entered.acquire();
    bool heartbeat = false;
    QTimer::singleShot(0, &controller, [&] { heartbeat = true; });
    QTRY_VERIFY(heartbeat);
    QVERIFY(controller.busy());
    QVERIFY(controller.copies().isEmpty());
    provider.release.release();
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(provider.listedPaths, QStringList {folder});
    QCOMPARE(controller.copies().size(), 2);
    QVERIFY(controller.copies().first().contains("20261002-copy"));
    QCOMPARE(controller.currentCopyIndex(), -1);
    QVERIFY(controller.entries().isEmpty());
    QVERIFY(provider.downloadedPaths.isEmpty());
    QVERIFY(provider.inspectedPaths.isEmpty());

    controller.setCopySearch("not verified");
    provider.blockInspect = true;
    controller.selectCopy(0);
    QVERIFY(controller.busy());
    QTRY_VERIFY(provider.entered.available() > 0);
    provider.entered.acquire();
    heartbeat = false;
    QTimer::singleShot(0, &controller, [&] { heartbeat = true; });
    QTRY_VERIFY(heartbeat);
    QVERIFY(controller.entries().isEmpty());
    provider.release.release();
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.currentCopyIndex(), 0);
    QCOMPARE(controller.copies().size(), 2);
    QCOMPARE(provider.downloadedPaths, QStringList {newest + "/manifest.json"});
    QCOMPARE(provider.inspectedPaths, QStringList {newest + "/nested/notes.txt"});
    QCOMPARE(controller.entries(), QStringList {"/source/notes.txt"});
    QCOMPARE(provider.listedPaths, QStringList {folder});
    QVERIFY(failures.isEmpty());

    // Changed files are rechecked instead of exposing stale verified entries.
    provider.blockInspect = false;
    QFile changed(remote.filePath(newest + "/nested/notes.txt"));
    QVERIFY(changed.open(QIODevice::WriteOnly));
    changed.write("other");
    changed.close();
    controller.selectCopy(0);
    QCOMPARE(controller.entries(), QStringList {"/source/notes.txt"});
    QVERIFY(controller.showingCachedData());
    QTRY_VERIFY(!controller.busy());
    QVERIFY(controller.entries().isEmpty());
    QVERIFY(!controller.showingCachedData());
    QCOMPARE(controller.unavailableEntries(), QStringList {"nested/notes.txt"});
    QCOMPARE(provider.downloadedPaths.size(), 2);
    QCOMPARE(provider.inspectedPaths.size(), 2);
}

void BackupRestoreControllerTest::discoveryFailureClearsPreviousResultsAndAllowsRetry()
{
    QTemporaryDir remote;
    QVERIFY(remote.isValid());
    const QString folder = "backups/computer/Documents";
    QVERIFY(createCopy(remote, folder + "/copy", "set-id", "copy"));
    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    BackupRestoreController controller(engine, &provider);
    QSignalSpy failures(&controller, &BackupRestoreController::failed);
    controller.discover(folder, "set-id");
    QTRY_VERIFY(!controller.busy());
    controller.selectCopy(0);
    QTRY_VERIFY(!controller.busy());
    QVERIFY(!controller.entries().isEmpty());
    controller.discover("backups/missing", "set-id");
    QVERIFY(controller.entries().isEmpty());
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(failures.count(), 1);
    QVERIFY(controller.copies().isEmpty());
    QCOMPARE(controller.currentCopyIndex(), -1);
    controller.discover(folder, "set-id");
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.copies().size(), 1);
}

void BackupRestoreControllerTest::rejectsUnidentifiableCopies_data()
{
    QTest::addColumn<QString>("manifestSetId");
    QTest::addColumn<QString>("manifestCopyId");
    QTest::addColumn<bool>("removeManifest");
    QTest::newRow("wrong backup") << "other-id" << "copy" << false;
    QTest::newRow("wrong copy") << "set-id" << "other-copy" << false;
    QTest::newRow("missing manifest") << "set-id" << "copy" << true;
}

void BackupRestoreControllerTest::rejectsUnidentifiableCopies()
{
    QFETCH(QString, manifestSetId);
    QFETCH(QString, manifestCopyId);
    QFETCH(bool, removeManifest);
    QTemporaryDir remote;
    QVERIFY(remote.isValid());
    const QString folder = "backups/computer/Documents";
    QVERIFY(createCopy(remote, folder + "/copy", manifestSetId, manifestCopyId));
    if (removeManifest) {
        QVERIFY(QFile::remove(remote.filePath(folder + "/copy/manifest.json")));
    }
    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    BackupRestoreController controller(engine, &provider);
    QSignalSpy failures(&controller, &BackupRestoreController::failed);
    controller.discover(folder, "set-id");
    QTRY_VERIFY(!controller.busy());
    controller.selectCopy(0);
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(failures.count(), 1);
    QVERIFY(controller.entries().isEmpty());
    QVERIFY(provider.inspectedPaths.isEmpty());
}

void BackupRestoreControllerTest::completesOnlyAfterAllSelectedFilesAreRestored_data()
{
    QTest::addColumn<QString>("failure");
    QTest::newRow("all files restored") << QString();
    QTest::newRow("second file missing") << QString("missing");
    QTest::newRow("second file corrupt") << QString("corrupt");
    QTest::newRow("second file placement fails") << QString("placement");
}

void BackupRestoreControllerTest::completesOnlyAfterAllSelectedFilesAreRestored()
{
    QFETCH(QString, failure);
    const bool failSecondFile = !failure.isEmpty();
    QTemporaryDir remote;
    QTemporaryDir destination;
    QTemporaryDir metadata;
    QVERIFY(remote.isValid());
    QVERIFY(destination.isValid());
    QVERIFY(metadata.isValid());
    QJsonArray entries;
    for (const QString &name : {QString("one.txt"), QString("two.txt")}) {
        QFile file(remote.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("notes");
        entries.append(QJsonObject {{"source", "/source/" + name}, {"remote", name}, {"size", 5},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash("notes", QCryptographicHash::Sha256).toHex())}});
    }
    QFile manifest(metadata.filePath("manifest.json"));
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(QJsonObject {{"version", 1}, {"entries", entries}}).toJson());
    manifest.close();
    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    BackupRestoreController controller(engine, &provider);
    controller.loadManifest(manifest.fileName());
    QSignalSpy completed(&controller, &BackupRestoreController::restoreCompleted);
    QSignalSpy failed(&controller, &BackupRestoreController::failed);
    if (failure == "placement") {
        QVERIFY(QDir().mkpath(destination.filePath("two.txt")));
    } else {
        QFile existing(destination.filePath("two.txt"));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("keep me"), qint64(7));
    }
    if (failure == "missing") {
        QVERIFY(QFile::remove(remote.filePath("two.txt")));
    } else if (failure == "corrupt") {
        QFile corrupt(remote.filePath("two.txt"));
        QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(corrupt.write("wrong"), qint64(5));
    }
    controller.restoreSelected({0, 1}, destination.path());
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(completed.count(), failSecondFile ? 0 : 1);
    QCOMPARE(failed.count(), failSecondFile ? 1 : 0);
    QCOMPARE(provider.downloadedPaths, (QStringList {"one.txt", "two.txt"}));
    QCOMPARE(controller.restoreProgress(), failSecondFile ? QString("1 of 2 files restored") : QString("2 of 2 files restored"));
    QCOMPARE(controller.restoreState(), failSecondFile ? QString("failed") : QString("succeeded"));
    QCOMPARE(controller.restoreIssues().size(), failSecondFile ? 1 : 0);
    if (failSecondFile) {
        const QVariantMap issue = controller.restoreIssues().first().toMap();
        QCOMPARE(issue.value("path").toString(), QString("two.txt"));
        QVERIFY(!issue.value("phase").toString().isEmpty());
        QCOMPARE(issue.value("reason").toString(), controller.restoreError());
        QVERIFY(!controller.restoreError().isEmpty());
    }
    if (failSecondFile) QVERIFY(!failed.first().first().toString().isEmpty());
    QFile first(destination.filePath("one.txt"));
    QVERIFY(first.open(QIODevice::ReadOnly));
    QCOMPARE(first.readAll(), QByteArray("notes"));
    if (failure == "placement") {
        QVERIFY(QFileInfo(destination.filePath("two.txt")).isDir());
    } else {
        QFile second(destination.filePath("two.txt"));
        QVERIFY(second.open(QIODevice::ReadOnly));
        QCOMPARE(second.readAll(), failSecondFile ? QByteArray("keep me") : QByteArray("notes"));
    }
}

void BackupRestoreControllerTest::restoresLegacySelections_data()
{
    QTest::addColumn<int>("version");
    QTest::addColumn<QString>("selection");
    for (int version : {1, 2}) {
        for (const QString &selection : {QString("single"), QString("multiple"), QString("folder"), QString("whole copy")}) {
            QTest::newRow(qPrintable(QString("version %1 / %2").arg(version).arg(selection))) << version << selection;
        }
    }
}

void BackupRestoreControllerTest::restoresLegacySelections()
{
    QFETCH(int, version);
    QFETCH(QString, selection);
    QTemporaryDir remote, destination;
    const QString copy = "backups/computer/Documents/copy";
    QVERIFY(createCopy(remote, copy, "documents", "copy", 4, version));
    // Keep an entry outside the selected folder to observe selective placement.
    const QString manifestPath = remote.filePath(copy + "/manifest.json");
    QFile manifest(manifestPath);
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    QJsonObject document = QJsonDocument::fromJson(manifest.readAll()).object();
    manifest.close();
    QJsonArray entries = document["entries"].toArray();
    QJsonObject outside = entries.at(3).toObject();
    outside["restore"] = "other/notes-3.txt";
    entries[3] = outside;
    document["entries"] = entries;
    document["expected"] = QJsonArray {"nested/notes-0.txt", "nested/notes-1.txt", "nested/notes-2.txt", "other/notes-3.txt"};
    QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(manifest.write(QJsonDocument(document).toJson()) > 0);
    manifest.close();

    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    BackupRestoreController controller(engine, &provider);
    controller.loadManifest(manifestPath);
    QCOMPARE(controller.entries().size(), 4);
    QCOMPARE(controller.downloadCost({0, 1, 2, 3}).value("bytes").toLongLong(), 20);
    QCOMPARE(controller.downloadCost({0, 0, -1, 99}).value("bytes").toLongLong(), 5);
    QCOMPARE(controller.downloadCost({0, 1}).value("archiveCount").toInt(), 0);
    QSignalSpy completed(&controller, &BackupRestoreController::restoreCompleted);
    QSignalSpy failed(&controller, &BackupRestoreController::failed);
    QVariantList indexes;
    if (selection == "single") {
        indexes = {1};
        controller.restore(1, destination.path());
    } else if (selection == "folder") {
        indexes = {0, 1, 2};
        controller.restoreFolder("nested", destination.path());
    } else {
        indexes = selection == "multiple" ? QVariantList {0, 2} : QVariantList {0, 1, 2, 3};
        controller.restoreSelected(indexes, destination.path());
    }
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(completed.count(), 1);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(provider.downloadedPaths.size(), indexes.size());
    QCOMPARE(controller.restoreProgressFraction(), 1.0);
    for (int index = 0; index < 4; ++index) {
        const QString relative = index == 3 ? QString("other/notes-3.txt") : QString("nested/notes-%1.txt").arg(index);
        QCOMPARE(QFile::exists(destination.filePath(relative)), indexes.contains(index));
        if (indexes.contains(index)) {
            QVERIFY(provider.downloadedPaths.contains(copy + QString("/nested/notes-%1.txt").arg(index)));
            QFile file(destination.filePath(relative));
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), QByteArray("notes"));
        }
    }
}

void BackupRestoreControllerTest::boundsProgressNotificationsForLargeSelections()
{
    QTemporaryDir remote, destination;
    const QString copy = "backups/computer/Documents/copy";
    const int fileCount = 500;
    QVERIFY(createCopy(remote, copy, "documents", "copy", fileCount));
    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    BackupRestoreController controller(engine, &provider);
    controller.loadManifest(remote.filePath(copy + "/manifest.json"));
    QVariantList selection;
    for (int index = 0; index < fileCount; ++index) selection.append(index);
    QSignalSpy progress(&controller, &BackupRestoreController::restoreProgressChanged);
    QSignalSpy completed(&controller, &BackupRestoreController::restoreCompleted);
    QElapsedTimer elapsed;
    elapsed.start();
    controller.restoreSelected(selection, destination.path());
    QTRY_VERIFY(!controller.busy());
    // Ten updates per second, plus initial/first/last/final notifications and
    // rounding slack; a fast large selection must not flood the GUI event loop.
    QVERIFY(progress.count() <= 6 + elapsed.elapsed() / 100);
    QCOMPARE(completed.count(), 1);
    QCOMPARE(provider.downloadedPaths.size(), fileCount);
    QCOMPARE(controller.restoreProgress(), QString("500 of 500 files restored"));
    QCOMPARE(controller.restoreProgressFraction(), 1.0);
    for (int index = 0; index < fileCount; ++index) {
        QFile file(destination.filePath(QString("nested/notes-%1.txt").arg(index)));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("notes"));
    }
}

void BackupRestoreControllerTest::restoresWithoutBlockingTheControllerThread()
{
    QTemporaryDir remote;
    QTemporaryDir destination;
    QTemporaryDir metadata;
    QVERIFY(remote.isValid());
    QVERIFY(destination.isValid());
    QVERIFY(metadata.isValid());
    QFile file(remote.filePath("notes.txt"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("notes");
    file.close();
    QFile manifest(metadata.filePath("manifest.json"));
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(R"({"version":1,"entries":[{"source":"/source/notes.txt","remote":"notes.txt","size":5,"sha256":"ab5aa97074c454a0632057e704220d9a6678fbf773a0a5806fc09b8173b07309"}]})");
    manifest.close();

    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    provider.blockDownload = true;
    BackupRestoreController controller(engine, &provider);
    controller.loadManifest(manifest.fileName());
    QSignalSpy completed(&controller, &BackupRestoreController::restoreCompleted);
    const auto unblock = qScopeGuard([&] { provider.release.release(10); });

    controller.restoreSelected({0}, destination.path());
    QVERIFY(controller.busy());
    QTRY_VERIFY(provider.entered.available() > 0);
    provider.entered.acquire();
    bool heartbeat = false;
    QTimer::singleShot(0, &controller, [&] { heartbeat = true; });
    QTRY_VERIFY(heartbeat);
    QVERIFY(controller.busy());
    QVERIFY(!QFile::exists(destination.filePath("notes.txt")));

    provider.release.release();
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(completed.count(), 1);
    QVERIFY(QFile::exists(destination.filePath("notes.txt")));
}

void BackupRestoreControllerTest::loadsAndRestoresSelectedEntry()
{
    QTemporaryDir remote;
    QTemporaryDir destination;
    QTemporaryDir manifestDirectory;
    QVERIFY(remote.isValid());
    QVERIFY(destination.isValid());
    QVERIFY(manifestDirectory.isValid());
    QDir().mkpath(remote.filePath(QStringLiteral("copy")));

    QFile file(remote.filePath(QStringLiteral("copy/notes.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("notes");
    file.close();

    QFile manifest(manifestDirectory.filePath(QStringLiteral("manifest.json")));
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(R"({"version":1,"entries":[{"source":"/source/notes.txt","remote":"copy/notes.txt","size":5,"sha256":"ab5aa97074c454a0632057e704220d9a6678fbf773a0a5806fc09b8173b07309"}]})");
    manifest.close();

    BackupEngine engine;
    LocalProvider provider(remote.path());
    BackupRestoreController controller(engine, &provider);
    controller.loadManifest(manifest.fileName());
    QCOMPARE(controller.entries(), QStringList {QStringLiteral("/source/notes.txt")});

    QSignalSpy statusSpy(&controller, &BackupRestoreController::statusChanged);
    controller.restore(0, destination.path());
    QTRY_VERIFY(!controller.busy());
    QVERIFY(!statusSpy.isEmpty());

    QFile restored(destination.filePath(QStringLiteral("notes.txt")));
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), QByteArray("notes"));
}

void BackupRestoreControllerTest::rejectsInvalidSelection()
{
    BackupEngine engine;
    BackupRestoreController controller(engine);
    QSignalSpy failureSpy(&controller, &BackupRestoreController::failed);

    controller.restore(0, QStringLiteral("/tmp"));

    QCOMPARE(failureSpy.count(), 1);
    QCOMPARE(failureSpy.first().at(0).toString(), QStringLiteral("No backup provider is configured."));
}

void BackupRestoreControllerTest::clearsEntriesWhenManifestFailsToLoad()
{
    QTemporaryDir manifestDirectory;
    QVERIFY(manifestDirectory.isValid());

    QFile manifest(manifestDirectory.filePath(QStringLiteral("manifest.json")));
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(R"({"version":1,"entries":[]})");
    manifest.close();

    BackupEngine engine;
    BackupRestoreController controller(engine);
    controller.loadManifest(manifest.fileName());
    QVERIFY(controller.entries().isEmpty());

    QSignalSpy failureSpy(&controller, &BackupRestoreController::failed);
    controller.loadManifest(manifestDirectory.filePath(QStringLiteral("missing.json")));

    QVERIFY(!failureSpy.isEmpty());
    QVERIFY(controller.entries().isEmpty());
}

void BackupRestoreControllerTest::rejectsEmptyDestination()
{
    BackupEngine engine;
    LocalProvider provider(QDir::homePath());
    BackupRestoreController controller(engine, &provider);
    QSignalSpy failureSpy(&controller, &BackupRestoreController::failed);

    controller.restore(0, QStringLiteral("  "));

    QCOMPARE(failureSpy.count(), 1);
    QCOMPARE(failureSpy.first().at(0).toString(), QStringLiteral("A restore destination folder is required."));
}

QTEST_MAIN(BackupRestoreControllerTest)
#include "backuprestorecontroller_test.moc"

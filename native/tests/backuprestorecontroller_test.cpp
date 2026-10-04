#include <QCryptographicHash>
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

static bool createCopy(const QTemporaryDir &remote, const QString &folder, const QString &setId, const QString &copyId, int fileCount = 1)
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
        && manifest.write(QJsonDocument(QJsonObject {{"version", 2}, {"application", "omacustos"},
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
    void restoresWithoutBlockingTheControllerThread();
    void navigationSupersedesDiscoveryWithoutPublishingOldResults();
    void navigationDuringRestoreKeepsTransferIndependent();
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

void BackupRestoreControllerTest::navigationDuringRestoreKeepsTransferIndependent()
{
    QTemporaryDir remote, destination;
    const QString first = "backups/computer/Documents";
    const QString second = "backups/computer/Photos";
    QVERIFY(createCopy(remote, first + "/copy", "documents", "copy"));
    QVERIFY(createCopy(remote, second + "/copy", "photos", "copy"));
    BackupEngine engine;
    RestoreTestProvider provider(remote.path());
    BackupRestoreController controller(engine, &provider);
    const auto unblock = qScopeGuard([&] { provider.release.release(10); });
    controller.discover(first, "documents");
    QTRY_VERIFY(!controller.busy());
    controller.selectCopy(0);
    QTRY_VERIFY(!controller.busy());
    provider.blockDownload = true;
    controller.restoreSelected({0}, destination.path());
    QTRY_VERIFY(provider.entered.available() > 0);
    provider.entered.acquire();
    QCOMPARE(controller.restoreProgressFraction(), 0.0);
    controller.discover(second, "photos");
    QVERIFY(controller.entries().isEmpty());
    QVERIFY(controller.property("restoring").toBool());
    QVERIFY(controller.property("browsing").toBool());
    bool heartbeat = false;
    QTimer::singleShot(0, &controller, [&] { heartbeat = true; });
    QTRY_VERIFY(heartbeat);
    provider.release.release(10);
    QTRY_VERIFY(!controller.busy());
    QVERIFY(QFile::exists(destination.filePath("nested/notes.txt")));
    QCOMPARE(provider.listedPaths.last(), second);
    QVERIFY(controller.entries().isEmpty());
    QCOMPARE(controller.property("restoreProgress").toString(), QString("1 of 1 files restored"));
    QCOMPARE(controller.restoreProgressFraction(), 1.0);
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
    QTest::addColumn<bool>("failSecondFile");
    QTest::newRow("all files restored") << false;
    QTest::newRow("second file fails") << true;
}

void BackupRestoreControllerTest::completesOnlyAfterAllSelectedFilesAreRestored()
{
    QFETCH(bool, failSecondFile);
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
    LocalProvider provider(remote.path());
    BackupRestoreController controller(engine, &provider);
    controller.loadManifest(manifest.fileName());
    QSignalSpy completed(&controller, &BackupRestoreController::restoreCompleted);
    QSignalSpy failed(&controller, &BackupRestoreController::failed);
    if (failSecondFile) {
        QVERIFY(QFile::remove(remote.filePath("two.txt")));
    }
    controller.restoreSelected({0, 1}, destination.path());
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(completed.count(), failSecondFile ? 0 : 1);
    QCOMPARE(failed.count(), failSecondFile ? 1 : 0);
    QCOMPARE(QFile::exists(destination.filePath("two.txt")), !failSecondFile);
    QVERIFY(QFile::exists(destination.filePath("one.txt")));
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

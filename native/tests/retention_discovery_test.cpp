#include <QTemporaryDir>
#include <QTest>
#include <QSignalSpy>

#include <algorithm>
#include <memory>
#include <functional>

#include "../src/backupcatalog.h"
#include "../src/backupengine.h"
#include "../src/backupecleanup.h"
#include "../src/backupsetcontroller.h"
#include "../src/localprovider.h"

class RecordingProvider final : public BackupProvider
{
public:
    QStringList calls;
    bool failPermanentDelete = false;
    std::unique_ptr<LocalProvider> local;
    bool failTrash = false;
    QString trashFailure = QStringLiteral("trash failed");
    QString deleteFailure = QStringLiteral("permanent delete failed");
    std::function<void()> beforeTrash;
    std::function<void()> beforeDelete;
    QStringList trashed;
    QStringList deleted;

    bool upload(const QString &, const QString &, QString *) override { return true; }
    bool ensureDirectory(const QString &, QString *) override { return true; }
    bool download(const QString &path, const QString &destination, QString *error) override
    {
        calls.append("download:" + path);
        return local ? local->download(path, destination, error) : true;
    }
    bool inspect(const QString &path, RemoteFile *file, QString *error) override
    {
        calls.append("inspect:" + path);
        return local ? local->inspect(path, file, error) : true;
    }
    bool list(const QString &path, QVector<RemoteItem> *items, QString *error) override
    {
        calls.append("list:" + path);
        return local ? local->list(path, items, error) : true;
    }
    bool trash(const QString &path, QString *error) override
    {
        if (beforeTrash) {
            beforeTrash();
        }
        calls.append(QStringLiteral("trash:%1").arg(path));
        if (trashed.contains(path)) {
            if (error != nullptr) {
                *error = QStringLiteral("target does not exist");
            }
            return false;
        }
        if (failTrash) {
            if (error != nullptr) {
                *error = trashFailure;
            }
            failTrash = false;
            return false;
        }
        trashed.append(path);
        return true;
    }
    bool permanentlyDelete(const QString &path, QString *error) override
    {
        if (beforeDelete) {
            beforeDelete();
        }
        calls.append(QStringLiteral("delete:%1").arg(path));
        if (deleted.contains(path)) {
            if (error != nullptr) {
                *error = QStringLiteral("target not found");
            }
            return false;
        }
        if (failPermanentDelete) {
            if (error != nullptr) {
                *error = deleteFailure;
            }
            failPermanentDelete = false;
            return false;
        }
        deleted.append(path);
        return true;
    }
};

class RetentionDiscoveryTest final : public QObject
{
    Q_OBJECT

private slots:
    void keepsNewestSuccessfulCopiesAndIncompleteCopiesAreEligible();
    void firstCleanupDecisionRemainsPendingUntilConfirmed();
    void cleanupUsesExactTargetsAndResumesAfterPermanentDeleteFailure();
    void refusesToDeleteAllowedRoot();
    void savedDecisionPersistsNewScopeBeforeTrash();
    void staleConfirmationPreservesOtherSetProgress();
    void changedProposalRequiresReview();
    void failedScopePersistenceDoesNotDelete();
    void trashFailureResumesWithoutExpandingScope();
    void activeCleanupBlocksCompetingWriter();
    void malformedStateDoesNotReplaceLoadedSnapshot();
    void phasePersistenceFailureResumesExactScope_data();
    void phasePersistenceFailureResumesExactScope();
    void uiRefreshesChangedProposalBeforeConfirmation();
    void failedConfirmationLeavesDecisionPending();
    void retentionFiltersProvenanceAndUnverifiedCopies();
    void providerOutageRetainsScopeAndPhase_data();
    void providerOutageRetainsScopeAndPhase();
    void forgettingDeletedCopyPreservesProgressAndInvalidatesReview();
    void discoversCompleteAndIncompleteCopiesWithoutLocalState();
    void scopedDiscoverySkipsOtherBackupsAndPreservesVerificationGates();
};

void RetentionDiscoveryTest::forgettingDeletedCopyPreservesProgressAndInvalidatesReview()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("cleanup.json"));
    const QString review = QStringLiteral("review");
    const QString running = QStringLiteral("running");
    const QString removed = QStringLiteral("computer/review/deleted");
    const QString remaining = QStringLiteral("computer/review/old");
    CleanupStore worker(path);
    QVERIFY(worker.propose(review, {removed, remaining}));
    QVERIFY(worker.propose(running, {QStringLiteral("computer/running/old")}));
    QVERIFY(worker.confirm(running, worker.state(running)));
    CleanupStore ui(path);
    QVERIFY(ui.load());
    const CleanupState presented = ui.state(review);
    RecordingProvider provider;
    provider.failPermanentDelete = true;
    QVERIFY(!BackupCleanup::apply(provider, worker, running));
    QVERIFY(ui.forgetTarget(review, removed));
    CleanupStore disk(path);
    QVERIFY(disk.load());
    QCOMPARE(disk.state(review).targets, QStringList {remaining});
    QCOMPARE(disk.state(running).trashed, QStringList {QStringLiteral("computer/running/old")});
    QCOMPARE(disk.state(running).lastError, QStringLiteral("permanent delete failed"));
    QString error;
    QVERIFY(!ui.confirm(review, presented, &error));
    QVERIFY(error.contains(QStringLiteral("Review the refreshed targets")));
    QVERIFY(ui.confirm(review, ui.state(review)));
}

void RetentionDiscoveryTest::scopedDiscoverySkipsOtherBackupsAndPreservesVerificationGates()
{
    QTemporaryDir source, remote;
    QVERIFY(QDir().mkpath(source.filePath("nested")));
    QFile file(source.filePath("nested/notes.txt"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("notes"), qint64(5));
    file.close();
    BackupEngine engine;
    LocalProvider local(remote.path());
    const QString root = "backups/computer/Documents";
    QString manifest, error;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const QString &copy : {QString("old"), QString("new"), QString("wrong-set")}) {
        const BackupCopyMetadata metadata {"computer", copy == "wrong-set" ? "other" : "documents",
            "Documents", copy, copy == "old" ? now.addDays(-1) : now};
        QVERIFY2(engine.backup({source.path()}, root + "/" + copy, {}, metadata, local, &manifest, &error), qPrintable(error));
        QDir(QFileInfo(manifest).absolutePath()).removeRecursively();
    }
    QVERIFY(QDir().mkpath(remote.filePath("backups/another-computer/Photos/copy")));
    RecordingProvider provider;
    provider.local = std::make_unique<LocalProvider>(remote.path());
    QVector<RemoteCopy> copies;
    QVERIFY2(BackupCatalog::discoverCopies(provider, root, "documents", &copies, &error), qPrintable(error));
    QCOMPARE(copies.size(), 2);
    QCOMPARE(BackupCleanup::eligibleTargets(copies, 1, "computer", "documents"), QStringList {root + "/old"});
    QCOMPARE(provider.calls.count("list:" + root), 1);
    QCOMPARE(provider.calls.size(), 6); // One listing, three manifests, two payload metadata checks.
    for (const QString &call : provider.calls) QVERIFY(!call.contains("another-computer"));
    QVERIFY(!provider.calls.contains("list:" + root + "/new/nested"));

    QVERIFY(QFile::remove(remote.filePath(root + "/new/nested/notes.txt")));
    provider.calls.clear();
    error.clear();
    QVERIFY(BackupCatalog::discoverCopies(provider, root, "documents", &copies, &error));
    // A missing payload in the newest copy cannot evict the surviving old copy.
    QVERIFY(BackupCleanup::eligibleTargets(copies, 1, "computer", "documents").isEmpty());
    QVERIFY(!copies.first().complete());
    QCOMPARE(copies.first().unavailableItems, QStringList {"nested/notes.txt"});
}

void RetentionDiscoveryTest::providerOutageRetainsScopeAndPhase_data()
{
    QTest::addColumn<bool>("afterTrash");
    QTest::newRow("trash-unavailable") << false;
    QTest::newRow("delete-unavailable") << true;
}

void RetentionDiscoveryTest::providerOutageRetainsScopeAndPhase()
{
    QFETCH(bool, afterTrash);
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("cleanup.json"));
    const QString setId = QStringLiteral("set-id");
    const QStringList targets {QStringLiteral("computer/set/old")};
    CleanupStore store(path);
    QVERIFY(store.propose(setId, targets));
    QVERIFY(store.confirm(setId, store.state(setId)));
    RecordingProvider provider;
    const QString outage = QStringLiteral("Service unavailable (HTTP 503)");
    provider.failTrash = !afterTrash;
    provider.failPermanentDelete = afterTrash;
    provider.trashFailure = outage;
    provider.deleteFailure = outage;
    QString error;
    QVERIFY(!BackupCleanup::apply(provider, store, setId, &error));
    QCOMPARE(error, outage);
    CleanupStore retry(path);
    QVERIFY(retry.load());
    QCOMPARE(retry.state(setId).targets, targets);
    QCOMPARE(retry.state(setId).trashed, afterTrash ? targets : QStringList());
    QVERIFY(retry.state(setId).completed.isEmpty());
    QCOMPARE(retry.state(setId).lastError, outage);
    const QStringList beforeRetry = afterTrash
        ? QStringList {QStringLiteral("trash:computer/set/old"), QStringLiteral("delete:computer/set/old")}
        : QStringList {QStringLiteral("trash:computer/set/old")};
    QCOMPARE(provider.calls, beforeRetry);
    QVERIFY(BackupCleanup::run(provider, retry, setId,
        {QStringLiteral("computer/set/old"), QStringLiteral("computer/set/new")}, QStringLiteral("computer/set")));
    QCOMPARE(provider.calls, beforeRetry + (afterTrash
        ? QStringList {QStringLiteral("delete:computer/set/old")}
        : QStringList {QStringLiteral("trash:computer/set/old"), QStringLiteral("delete:computer/set/old")}));
}

void RetentionDiscoveryTest::phasePersistenceFailureResumesExactScope_data()
{
    QTest::addColumn<bool>("afterDelete");
    QTest::newRow("after-trash") << false;
    QTest::newRow("after-permanent-delete") << true;
}

void RetentionDiscoveryTest::phasePersistenceFailureResumesExactScope()
{
    QFETCH(bool, afterDelete);
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("cleanup.json"));
    const QString setId = QStringLiteral("set-id");
    const QStringList targets {QStringLiteral("computer/set/old")};
    CleanupStore store(path);
    QVERIFY(store.propose(setId, targets));
    QVERIFY(store.confirm(setId, store.state(setId)));
    const QFile::Permissions permissions = QFile::permissions(path);
    RecordingProvider provider;
    const auto interruptPersistence = [&] {
        CleanupStore disk(path);
        QVERIFY(disk.load());
        QCOMPARE(disk.state(setId).targets, targets);
        QCOMPARE(disk.state(setId).decision, QStringLiteral("confirmed"));
        if (afterDelete) {
            QCOMPARE(disk.state(setId).trashed, targets);
        }
        QVERIFY(QFile::setPermissions(path, QFile::ReadOwner));
    };
    if (afterDelete) {
        provider.beforeDelete = interruptPersistence;
    } else {
        provider.beforeTrash = interruptPersistence;
    }
    QString error;
    const bool result = BackupCleanup::apply(provider, store, setId, &error);
    QVERIFY(QFile::setPermissions(path, permissions));
    QVERIFY(!result);
    QVERIFY(error.contains(QStringLiteral("Unable to write the cleanup state")));
    provider.beforeTrash = {};
    provider.beforeDelete = {};
    CleanupStore retry(path);
    QVERIFY(retry.load());
    QCOMPARE(retry.state(setId).targets, targets);
    QCOMPARE(retry.state(setId).trashed, afterDelete ? targets : QStringList());
    QVERIFY(retry.state(setId).completed.isEmpty());
    QVERIFY(BackupCleanup::run(provider, retry, setId,
        {QStringLiteral("computer/set/old"), QStringLiteral("computer/set/new")}, QStringLiteral("computer/set")));
    const QStringList expected = afterDelete
        ? QStringList {QStringLiteral("trash:computer/set/old"), QStringLiteral("delete:computer/set/old"), QStringLiteral("delete:computer/set/old")}
        : QStringList {QStringLiteral("trash:computer/set/old"), QStringLiteral("trash:computer/set/old"), QStringLiteral("delete:computer/set/old")};
    QCOMPARE(provider.calls, expected);
    QVERIFY(retry.load());
    QVERIFY(retry.state(setId).targets.isEmpty());
}

void RetentionDiscoveryTest::uiRefreshesChangedProposalBeforeConfirmation()
{
    QTemporaryDir directory;
    const QString configPath = directory.filePath(QStringLiteral("config.json"));
    BackupConfig config;
    BackupSet set;
    set.id = QStringLiteral("set-id");
    set.name = QStringLiteral("Set");
    set.remoteRoot = QStringLiteral("/backups");
    set.sourceDirectories = {directory.path()};
    config.sets.append(set);
    QVERIFY(BackupConfigStore(configPath).save(config));
    CleanupStore worker(directory.filePath(QStringLiteral("omacustos-backup-cleanup.json")));
    const QStringList original {QStringLiteral("computer/set/old")};
    QVERIFY(worker.propose(set.id, original));
    BackupEngine engine;
    BackupSetController ui(engine, configPath);
    QCOMPARE(ui.cleanupTargets(), original);
    QVERIFY(ui.cleanupConfirmationRequired());
    QSignalSpy refreshed(&ui, &BackupSetController::cleanupChanged);
    QSignalSpy failed(&ui, &BackupSetController::failed);
    const QStringList updated {QStringLiteral("computer/set/new")};
    QVERIFY(worker.propose(set.id, updated));
    QVERIFY(!ui.confirmCleanup());
    QCOMPARE(refreshed.size(), 1);
    QCOMPARE(failed.size(), 1);
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("Review the refreshed targets")));
    QCOMPARE(ui.cleanupTargets(), updated);
    QVERIFY(ui.cleanupConfirmationRequired());
    QVERIFY(worker.load());
    QCOMPARE(worker.state(set.id).decision, QStringLiteral("pending"));
    QVERIFY(ui.confirmCleanup());
    QVERIFY(!ui.cleanupConfirmationRequired());
    QVERIFY(worker.load());
    QCOMPARE(worker.state(set.id).decision, QStringLiteral("confirmed"));
    QCOMPARE(worker.state(set.id).targets, updated);
}

void RetentionDiscoveryTest::failedConfirmationLeavesDecisionPending()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("cleanup.json"));
    CleanupStore store(path);
    const QString setId = QStringLiteral("set-id");
    QVERIFY(store.propose(setId, {QStringLiteral("computer/set/old")}));
    const CleanupState presented = store.state(setId);
    const QFile::Permissions permissions = QFile::permissions(path);
    QVERIFY(QFile::setPermissions(path, QFile::ReadOwner));
    QString error;
    const bool result = store.confirm(setId, presented, &error);
    QVERIFY(QFile::setPermissions(path, permissions));
    QVERIFY(!result);
    QVERIFY(error.contains(QStringLiteral("Unable to write the cleanup state")));
    QCOMPARE(store.state(setId).decision, QStringLiteral("pending"));
    RecordingProvider provider;
    QVERIFY(BackupCleanup::apply(provider, store, setId));
    QVERIFY(provider.calls.isEmpty());
}

void RetentionDiscoveryTest::retentionFiltersProvenanceAndUnverifiedCopies()
{
    const QDateTime base = QDateTime::currentDateTimeUtc();
    QVector<RemoteCopy> copies;
    RemoteCopy copy;
    copy.computerName = QStringLiteral("computer");
    copy.setId = QStringLiteral("set-id");
    copy.status = QStringLiteral("complete");
    copy.rootPath = QStringLiteral("computer/set/old");
    copy.createdAt = base.addDays(-1);
    copies.append(copy);
    copy.rootPath = QStringLiteral("computer/set/current");
    copy.createdAt = base;
    copies.append(copy);
    copy.rootPath = QStringLiteral("computer/set/unverified");
    copy.createdAt = base.addDays(1);
    copy.unavailableItems = {QStringLiteral("missing.txt")};
    copies.append(copy);
    copy.unavailableItems.clear();
    copy.rootPath = QStringLiteral("computer/set/incomplete");
    copy.status = QStringLiteral("incomplete");
    copies.append(copy);
    copy.rootPath = QStringLiteral("other-computer/set/incomplete");
    copy.computerName = QStringLiteral("other-computer");
    copies.append(copy);
    copy.rootPath = QStringLiteral("computer/other-set/incomplete");
    copy.computerName = QStringLiteral("computer");
    copy.setId = QStringLiteral("other-set");
    copies.append(copy);
    QCOMPARE(BackupCleanup::eligibleTargets(copies, 2, QStringLiteral("computer"), QStringLiteral("set-id")),
        QStringList {QStringLiteral("computer/set/incomplete")});
    QCOMPARE(BackupCleanup::eligibleTargets(copies, 1, QStringLiteral("computer"), QStringLiteral("set-id")),
        (QStringList {QStringLiteral("computer/set/incomplete"), QStringLiteral("computer/set/old")}));
}

void RetentionDiscoveryTest::savedDecisionPersistsNewScopeBeforeTrash()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("cleanup.json"));
    CleanupStore worker(path);
    QVERIFY(worker.propose(QStringLiteral("set-id"), {QStringLiteral("computer/set/first")}));
    QVERIFY(worker.confirm(QStringLiteral("set-id"), worker.state(QStringLiteral("set-id"))));
    RecordingProvider provider;
    QVERIFY(BackupCleanup::apply(provider, worker, QStringLiteral("set-id")));
    const QStringList targets {QStringLiteral("computer/set/next")};
    provider.beforeTrash = [&] {
        CleanupStore disk(path);
        QVERIFY(disk.load());
        QCOMPARE(disk.state(QStringLiteral("set-id")).decision, QStringLiteral("confirmed"));
        QCOMPARE(disk.state(QStringLiteral("set-id")).targets, targets);
    };
    QVERIFY(BackupCleanup::run(provider, worker, QStringLiteral("set-id"), targets, QStringLiteral("computer/set")));
}

void RetentionDiscoveryTest::staleConfirmationPreservesOtherSetProgress()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("cleanup.json"));
    CleanupStore worker(path);
    QVERIFY(worker.propose(QStringLiteral("review"), {QStringLiteral("computer/review/old")}));
    QVERIFY(worker.propose(QStringLiteral("running"), {QStringLiteral("computer/running/old")}));
    QVERIFY(worker.confirm(QStringLiteral("running"), worker.state(QStringLiteral("running"))));
    CleanupStore ui(path);
    QVERIFY(ui.load());
    RecordingProvider provider;
    provider.failPermanentDelete = true;
    QVERIFY(!BackupCleanup::apply(provider, worker, QStringLiteral("running")));
    QVERIFY(ui.confirm(QStringLiteral("review"), ui.state(QStringLiteral("review"))));
    CleanupStore disk(path);
    QVERIFY(disk.load());
    QCOMPARE(disk.state(QStringLiteral("running")).trashed,
        QStringList {QStringLiteral("computer/running/old")});
    QCOMPARE(disk.state(QStringLiteral("running")).lastError, QStringLiteral("permanent delete failed"));
    QCOMPARE(disk.state(QStringLiteral("review")).decision, QStringLiteral("confirmed"));

    // The worker still has its pre-confirmation snapshot; its retry must also
    // preserve the UI's newer decision, without adding newly discovered targets.
    QVERIFY(BackupCleanup::run(provider, worker, QStringLiteral("running"),
        {QStringLiteral("computer/running/old"), QStringLiteral("computer/running/new")}, QStringLiteral("computer/running")));
    QVERIFY(disk.load());
    QCOMPARE(disk.state(QStringLiteral("review")).decision, QStringLiteral("confirmed"));
    QCOMPARE(provider.calls, (QStringList {QStringLiteral("trash:computer/running/old"),
        QStringLiteral("delete:computer/running/old"), QStringLiteral("delete:computer/running/old")}));
}

void RetentionDiscoveryTest::changedProposalRequiresReview()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("cleanup.json"));
    CleanupStore worker(path);
    const QString setId = QStringLiteral("set-id");
    QVERIFY(worker.propose(setId, {QStringLiteral("computer/set/old")}));
    CleanupStore ui(path);
    QVERIFY(ui.load());
    const CleanupState presented = ui.state(setId);
    const QStringList updated {QStringLiteral("computer/set/new")};
    QVERIFY(worker.propose(setId, updated));
    QString error;
    QVERIFY(!ui.confirm(setId, presented, &error));
    QVERIFY(error.contains(QStringLiteral("Review the refreshed targets")));
    QCOMPARE(ui.state(setId).targets, updated);
    QCOMPARE(ui.state(setId).decision, QStringLiteral("pending"));
    CleanupStore disk(path);
    QVERIFY(disk.load());
    QCOMPARE(disk.state(setId).decision, QStringLiteral("pending"));
    // Even the same paths in a later proposal must not match an old review.
    QVERIFY(worker.propose(setId, presented.targets));
    QVERIFY(!ui.confirm(setId, presented, &error));
    QCOMPARE(ui.state(setId).targets, presented.targets);
    QVERIFY(ui.confirm(setId, ui.state(setId)));
}

void RetentionDiscoveryTest::failedScopePersistenceDoesNotDelete()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("cleanup.json"));
    CleanupStore store(path);
    const QString setId = QStringLiteral("set-id");
    QVERIFY(store.propose(setId, {QStringLiteral("computer/set/first")}));
    QVERIFY(store.confirm(setId, store.state(setId)));
    RecordingProvider provider;
    QVERIFY(BackupCleanup::apply(provider, store, setId));
    provider.calls.clear();
    const QFile::Permissions permissions = QFile::permissions(path);
    QVERIFY(QFile::setPermissions(path, QFile::ReadOwner));
    QString error;
    const bool result = BackupCleanup::run(provider, store, setId,
        {QStringLiteral("computer/set/next")}, QStringLiteral("computer/set"), &error);
    QVERIFY(QFile::setPermissions(path, permissions));
    QVERIFY(!result);
    QVERIFY(error.contains(QStringLiteral("Unable to write the cleanup state")));
    QVERIFY(error.contains(QStringLiteral("retry")));
    QVERIFY(provider.calls.isEmpty());
    CleanupStore disk(path);
    QVERIFY(disk.load());
    QVERIFY(disk.state(setId).targets.isEmpty());
    QVERIFY(store.state(setId).targets.isEmpty());
}

void RetentionDiscoveryTest::trashFailureResumesWithoutExpandingScope()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("cleanup.json"));
    CleanupStore store(path);
    const QString setId = QStringLiteral("set-id");
    const QStringList original {QStringLiteral("computer/set/old")};
    QVERIFY(store.propose(setId, original));
    QVERIFY(store.confirm(setId, store.state(setId)));
    RecordingProvider provider;
    provider.failTrash = true;
    QVERIFY(!BackupCleanup::apply(provider, store, setId));
    CleanupStore disk(path);
    QVERIFY(disk.load());
    QCOMPARE(disk.state(setId).targets, original);
    QVERIFY(disk.state(setId).trashed.isEmpty());
    QCOMPARE(disk.state(setId).lastError, QStringLiteral("trash failed"));
    QVERIFY(BackupCleanup::run(provider, disk, setId,
        {QStringLiteral("computer/set/old"), QStringLiteral("computer/set/new")}, QStringLiteral("computer/set")));
    QCOMPARE(provider.calls, (QStringList {QStringLiteral("trash:computer/set/old"),
        QStringLiteral("trash:computer/set/old"), QStringLiteral("delete:computer/set/old")}));
}

void RetentionDiscoveryTest::activeCleanupBlocksCompetingWriter()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("cleanup.json"));
    CleanupStore worker(path);
    QVERIFY(worker.propose(QStringLiteral("running"), {QStringLiteral("computer/running/old")}));
    QVERIFY(worker.confirm(QStringLiteral("running"), worker.state(QStringLiteral("running"))));
    QVERIFY(worker.propose(QStringLiteral("review"), {QStringLiteral("computer/review/old")}));
    CleanupStore ui(path);
    QVERIFY(ui.load());
    const CleanupState presented = ui.state(QStringLiteral("review"));
    RecordingProvider provider;
    provider.beforeTrash = [&] {
        QString error;
        QVERIFY(!ui.confirm(QStringLiteral("review"), presented, &error));
        QVERIFY(error.contains(QStringLiteral("Wait for active cleanup")));
    };
    QVERIFY(BackupCleanup::apply(provider, worker, QStringLiteral("running")));
    QVERIFY(ui.confirm(QStringLiteral("review"), presented));
    CleanupStore disk(path);
    QVERIFY(disk.load());
    QVERIFY(disk.state(QStringLiteral("running")).targets.isEmpty());
    QCOMPARE(disk.state(QStringLiteral("review")).decision, QStringLiteral("confirmed"));
}

void RetentionDiscoveryTest::malformedStateDoesNotReplaceLoadedSnapshot()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("cleanup.json"));
    CleanupStore store(path);
    const QStringList targets {QStringLiteral("computer/set/old")};
    QVERIFY(store.propose(QStringLiteral("set-id"), targets));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{\"sets\":{\"a-valid\":{},\"z-invalid\":42}}");
    file.close();
    QVERIFY(!store.load());
    QCOMPARE(store.state(QStringLiteral("set-id")).targets, targets);
    QString error;
    QVERIFY(!store.confirm(QStringLiteral("set-id"), store.state(QStringLiteral("set-id")), &error));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("{\"sets\":{\"a-valid\":{},\"z-invalid\":42}}"));
}

void RetentionDiscoveryTest::keepsNewestSuccessfulCopiesAndIncompleteCopiesAreEligible()
{
    const QDateTime base(QDate(2026, 9, 1), QTime(12, 0), Qt::UTC);
    QVector<RemoteCopy> copies;
    for (int index = 0; index < 4; ++index) {
        copies.append({
            QStringLiteral("computer/set/copy-%1").arg(index), {}, QStringLiteral("computer"), QStringLiteral("set-id"),
            QStringLiteral("Set"), QStringLiteral("copy-%1").arg(index), QStringLiteral("complete"), base.addDays(index), {}, {}, {},
        });
    }
    copies.append({
        QStringLiteral("computer/set/incomplete"), {}, QStringLiteral("computer"), QStringLiteral("set-id"),
        QStringLiteral("Set"), QStringLiteral("incomplete"), QStringLiteral("incomplete"), base.addDays(5), {}, {}, {},
    });

    const QStringList expectedTargets {
        QStringLiteral("computer/set/copy-0"),
        QStringLiteral("computer/set/incomplete"),
    };
    QCOMPARE(BackupCleanup::eligibleTargets(copies, 3), expectedTargets);
}

void RetentionDiscoveryTest::cleanupUsesExactTargetsAndResumesAfterPermanentDeleteFailure()
{
    QTemporaryDir stateDirectory;
    QVERIFY(stateDirectory.isValid());
    CleanupStore store(stateDirectory.filePath(QStringLiteral("cleanup.json")));
    QVERIFY(store.propose(QStringLiteral("set-id"), {QStringLiteral("computer/set/old")}));
    QVERIFY(store.confirm(QStringLiteral("set-id"), store.state(QStringLiteral("set-id"))));

    RecordingProvider provider;
    provider.failPermanentDelete = true;
    QString error;
    QVERIFY(!BackupCleanup::apply(provider, store, QStringLiteral("set-id"), &error));
    const QStringList firstCalls {
        QStringLiteral("trash:computer/set/old"),
        QStringLiteral("delete:computer/set/old"),
    };
    QCOMPARE(provider.calls, firstCalls);
    QVERIFY(error.contains(QStringLiteral("permanent delete failed")));

    CleanupStore reloaded(stateDirectory.filePath(QStringLiteral("cleanup.json")));
    QVERIFY(reloaded.load(&error));
    QVERIFY(BackupCleanup::apply(provider, reloaded, QStringLiteral("set-id"), &error));
    const QStringList allCalls {
        QStringLiteral("trash:computer/set/old"),
        QStringLiteral("delete:computer/set/old"),
        QStringLiteral("delete:computer/set/old"),
    };
    QCOMPARE(provider.calls, allCalls);
    QVERIFY(reloaded.state(QStringLiteral("set-id")).targets.isEmpty());
}

void RetentionDiscoveryTest::refusesToDeleteAllowedRoot()
{
    QTemporaryDir stateDirectory;
    QVERIFY(stateDirectory.isValid());
    CleanupStore store(stateDirectory.filePath(QStringLiteral("cleanup.json")));
    QVERIFY(store.propose(QStringLiteral("set-id"), {QStringLiteral("computer/set")}));
    QVERIFY(store.confirm(QStringLiteral("set-id"), store.state(QStringLiteral("set-id"))));

    RecordingProvider provider;
    QString error;
    QVERIFY(!BackupCleanup::apply(provider, store, QStringLiteral("set-id"), QStringLiteral("computer/set"), &error));
    QCOMPARE(error, QStringLiteral("The cleanup target is outside the configured backup."));
    QVERIFY(provider.calls.isEmpty());
}

void RetentionDiscoveryTest::firstCleanupDecisionRemainsPendingUntilConfirmed()
{
    QTemporaryDir stateDirectory;
    QVERIFY(stateDirectory.isValid());
    const QString path = stateDirectory.filePath(QStringLiteral("cleanup.json"));
    CleanupStore store(path);
    const QStringList targets {QStringLiteral("computer/set/old")};
    RecordingProvider provider;
    QVERIFY(BackupCleanup::run(provider, store, QStringLiteral("set-id"), targets, QStringLiteral("computer/set")));
    QVERIFY(provider.calls.isEmpty());

    CleanupStore reloaded(path);
    QVERIFY(reloaded.load());
    QCOMPARE(reloaded.state(QStringLiteral("set-id")).decision, QStringLiteral("pending"));
    const QStringList expectedTargets {QStringLiteral("computer/set/old")};
    QCOMPARE(reloaded.state(QStringLiteral("set-id")).targets, expectedTargets);
    QVERIFY(reloaded.confirm(QStringLiteral("set-id"), reloaded.state(QStringLiteral("set-id"))));

    CleanupStore confirmed(path);
    QVERIFY(confirmed.load());
    QCOMPARE(confirmed.state(QStringLiteral("set-id")).decision, QStringLiteral("confirmed"));
}

void RetentionDiscoveryTest::discoversCompleteAndIncompleteCopiesWithoutLocalState()
{
    QTemporaryDir source;
    QTemporaryDir remote;
    QVERIFY(source.isValid());
    QVERIFY(remote.isValid());
    QFile file(source.filePath(QStringLiteral("notes.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("notes");
    file.close();

    BackupEngine engine;
    LocalProvider provider(remote.path());
    const BackupCopyMetadata completeMetadata {
        QStringLiteral("old-computer"), QStringLiteral("set-id"), QStringLiteral("Documents"),
        QStringLiteral("complete-copy"), QDateTime::currentDateTimeUtc().addDays(-1),
    };
    QString manifestPath;
    QString error;
    QVERIFY(engine.backup({source.path()}, QStringLiteral("backups/old-computer/Documents/complete-copy"), {}, completeMetadata, provider, &manifestPath, &error));

    const BackupCopyMetadata incompleteMetadata {
        QStringLiteral("old-computer"), QStringLiteral("set-id"), QStringLiteral("Documents"),
        QStringLiteral("incomplete-copy"), QDateTime::currentDateTimeUtc(),
    };
    QVERIFY(!engine.backup(
        {source.path(), source.filePath(QStringLiteral("missing.txt"))},
        QStringLiteral("backups/old-computer/Documents/incomplete-copy"), {}, incompleteMetadata, provider, &manifestPath, &error));

    const BackupCopyMetadata otherComputerMetadata {
        QStringLiteral("other-computer"), QStringLiteral("other-set-id"), QStringLiteral("Documents"),
        QStringLiteral("complete-copy"), QDateTime::currentDateTimeUtc().addDays(-2),
    };
    QVERIFY(engine.backup(
        {source.path()}, QStringLiteral("backups/other-computer/Documents/complete-copy"), {},
        otherComputerMetadata, provider, &manifestPath, &error));

    QVERIFY(QDir().mkpath(remote.filePath(QStringLiteral("backups/unrelated"))));
    QFile unrelated(remote.filePath(QStringLiteral("backups/unrelated/personal.txt")));
    QVERIFY(unrelated.open(QIODevice::WriteOnly));
    unrelated.write("do not delete");
    unrelated.close();

    QVector<RemoteCopy> copies;
    QVERIFY(BackupCatalog::discover(provider, QStringLiteral("backups"), &copies, &error));
    QCOMPARE(copies.size(), 3);
    QVERIFY(std::any_of(copies.cbegin(), copies.cend(), [](const RemoteCopy &copy) {
        return copy.computerName == QStringLiteral("other-computer") && copy.setId == QStringLiteral("other-set-id");
    }));
    const auto incomplete = std::find_if(copies.cbegin(), copies.cend(), [](const RemoteCopy &copy) {
        return copy.status == QStringLiteral("incomplete");
    });
    QVERIFY(incomplete != copies.cend());
    QCOMPARE(incomplete->setId, QStringLiteral("set-id"));
    QCOMPARE(incomplete->entries.size(), 1);
    QCOMPARE(incomplete->failedItems.size(), 1);
    QVERIFY(incomplete->failedItems.first().endsWith(QStringLiteral("/missing.txt")));
}

QTEST_MAIN(RetentionDiscoveryTest)
#include "retention_discovery_test.moc"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "../src/backupconfig.h"
#include "../src/backupecleanup.h"
#include "../src/backupengine.h"
#include "../src/backuprunstore.h"
#include "../src/localprovider.h"
#include "../src/recentbackupcopies.h"

class CopyProvider final : public BackupProvider
{
public:
    explicit CopyProvider(const QString &root) : local(root) {}
    LocalProvider local;
    QStringList trashed;
    QStringList downloaded;
    QStringList listed;
    bool upload(const QString &a, const QString &b, QString *e) override { return local.upload(a, b, e); }
    bool ensureDirectory(const QString &p, QString *e) override { return local.ensureDirectory(p, e); }
    bool download(const QString &a, const QString &b, QString *e) override { downloaded.append(a); return local.download(a, b, e); }
    bool inspect(const QString &p, RemoteFile *f, QString *e) override { return local.inspect(p, f, e); }
    bool list(const QString &p, QVector<RemoteItem> *i, QString *e) override { listed.append(p); return local.list(p, i, e); }
    bool trash(const QString &p, QString *e) override { trashed.append(p); return local.trash(p, e); }
    bool permanentlyDelete(const QString &, QString *) override { return false; }
};

struct CopyFixture {
    QTemporaryDir state;
    QTemporaryDir source;
    QTemporaryDir remote;
    BackupEngine engine;
    CopyProvider provider {remote.path()};
    BackupSet set {QStringLiteral("documents-id"), QStringLiteral("Documents"), QStringLiteral("backups"), {source.path()}, {}};
    QString configPath = state.filePath(QStringLiteral("settings.json"));
    QString runPath = state.filePath(QStringLiteral("omacustos-backup-runs.json"));
    QDateTime date {QDate(2026, 10, 2), QTime(9, 30)};

    bool prepare()
    {
        QFile file(source.filePath(QStringLiteral("notes.txt")));
        if (!file.open(QIODevice::WriteOnly)) return false;
        file.write("important notes");
        file.close();
        BackupConfig config;
        config.sets = {set};
        BackupRunStore runs(runPath);
        runs.ensureSet(set.id);
        runs.markSuccess(*runs.find(set.id), date);
        runs.find(set.id)->nextScheduled = date.addDays(1);
        return BackupConfigStore(configPath).save(config) && runs.save();
    }

    QString path(const QString &id) const { return QDir(set.remoteFolder(QStringLiteral("computer"))).filePath(id); }

    bool copy(const QString &id, int days, const QString &setId = QStringLiteral("documents-id"))
    {
        const BackupCopyMetadata metadata {QStringLiteral("computer"), setId, set.name, id, date.addDays(days)};
        return engine.backup(set.sourceDirectories, path(id), {}, metadata, provider, nullptr);
    }
};

class RecentBackupCopiesTest final : public QObject
{
    Q_OBJECT
private slots:
    void confirmationDeletesOnlyItsExactCopyAndKeepsTheSet();
    void persistedCopyPathControlsBrowsingAndDeletion();
    void changedRunPointerPreventsDeletion();
    void foreignManifestsAndUnsafePathsCannotBeDeleted();
    void runningWorkerPreventsDeletion();
    void browsingRecordedCopyNeedsNoManifestOrWorkerLock();
    void browsingRejectsUnsafeRecordedPath();
};

void RecentBackupCopiesTest::confirmationDeletesOnlyItsExactCopyAndKeepsTheSet()
{
    CopyFixture fixture;
    QVERIFY(fixture.prepare());
    QVERIFY(fixture.copy(QStringLiteral("old"), -1));
    QVERIFY(fixture.copy(QStringLiteral("recent"), 0));
    CleanupStore cleanup(fixture.state.filePath(QStringLiteral("omacustos-backup-cleanup.json")));
    QVERIFY(cleanup.propose(fixture.set.id, {fixture.path(QStringLiteral("old")), fixture.path(QStringLiteral("recent"))}));
    RecentBackupCopies copies(fixture.provider, fixture.configPath, QStringLiteral("computer"));
    QSignalSpy warning(&copies, &RecentBackupCopies::deleteConfirmationReady);
    QSignalSpy deleted(&copies, &RecentBackupCopies::copyDeleted);
    QSignalSpy failure(&copies, &RecentBackupCopies::failed);

    copies.requestDelete(fixture.set.id);
    QVERIFY(copies.deletingSetId().isEmpty());
    QTRY_COMPARE(warning.count(), 1);
    QCOMPARE(warning.first().at(1).toString(), fixture.path(QStringLiteral("recent")));
    QVERIFY(fixture.provider.trashed.isEmpty());
    copies.cancelDelete();
    copies.confirmDelete();
    QVERIFY(!copies.busy());
    QVERIFY(fixture.provider.trashed.isEmpty());

    copies.requestDelete(fixture.set.id);
    QTRY_COMPARE(warning.count(), 2);
    QVERIFY(fixture.copy(QStringLiteral("newer"), 1));
    copies.confirmDelete();
    QCOMPARE(copies.deletingSetId(), fixture.set.id);
    QTRY_COMPARE(deleted.count(), 1);
    QVERIFY(copies.deletingSetId().isEmpty());
    QVERIFY(failure.isEmpty());
    QCOMPARE(fixture.provider.trashed, QStringList {fixture.path(QStringLiteral("recent"))});
    QVERIFY(!QFileInfo::exists(fixture.remote.filePath(fixture.path(QStringLiteral("recent")))));
    QVERIFY(QFileInfo::exists(fixture.remote.filePath(fixture.path(QStringLiteral("old")))));
    QVERIFY(QFileInfo::exists(fixture.remote.filePath(fixture.path(QStringLiteral("newer")))));
    BackupConfig config;
    QVERIFY(BackupConfigStore(fixture.configPath).load(&config));
    QCOMPARE(config.sets.size(), 1);
    BackupRunStore runs(fixture.runPath);
    QVERIFY(runs.load());
    QCOMPARE(runs.find(fixture.set.id)->status, QStringLiteral("copy_deleted"));
    QVERIFY(runs.find(fixture.set.id)->remoteCopyPath.isEmpty());
    QCOMPARE(runs.find(fixture.set.id)->nextScheduled, fixture.date.addDays(1));
    QVERIFY(runs.readyIndexes(fixture.date.addDays(2)).isEmpty());
    QVERIFY(cleanup.load());
    QCOMPARE(cleanup.state(fixture.set.id).targets, QStringList {fixture.path(QStringLiteral("old"))});
    QCOMPARE(cleanup.state(fixture.set.id).decision, QStringLiteral("pending"));
}

void RecentBackupCopiesTest::persistedCopyPathControlsBrowsingAndDeletion()
{
    CopyFixture fixture;
    QVERIFY(fixture.prepare());
    QVERIFY(fixture.copy(QStringLiteral("pointed-copy"), -1));
    QVERIFY(fixture.copy(QStringLiteral("newer-copy"), 0));
    BackupRunStore runs(fixture.runPath);
    QVERIFY(runs.load());
    runs.find(fixture.set.id)->remoteCopyPath = fixture.path(QStringLiteral("pointed-copy"));
    QVERIFY(runs.save());
    RecentBackupCopies copies(fixture.provider, fixture.configPath, QStringLiteral("computer"));
    QSignalSpy opened(&copies, &RecentBackupCopies::folderResolved);
    QSignalSpy warning(&copies, &RecentBackupCopies::deleteConfirmationReady);
    copies.openCopy(fixture.set.id);
    QTRY_COMPARE(opened.count(), 1);
    QCOMPARE(opened.first().first().toString(), fixture.path(QStringLiteral("pointed-copy")));
    QVERIFY(fixture.provider.downloaded.isEmpty());
    copies.requestDelete(fixture.set.id);
    QTRY_COMPARE(warning.count(), 1);
    QCOMPARE(warning.first().at(1).toString(), opened.first().first().toString());
    QVERIFY(fixture.provider.trashed.isEmpty());
}

void RecentBackupCopiesTest::browsingRecordedCopyNeedsNoManifestOrWorkerLock()
{
    CopyFixture fixture;
    QVERIFY(fixture.prepare());
    BackupRunStore runs(fixture.runPath);
    QVERIFY(runs.load());
    runs.find(fixture.set.id)->remoteCopyPath = fixture.path(QStringLiteral("recorded-copy"));
    QVERIFY(runs.save());
    // No remote folder or manifest exists. Browsing passes the known path to
    // the URL resolver; deletion must still verify the remote manifest.
    QLockFile workerLock(fixture.configPath + QStringLiteral(".worker.lock"));
    QLockFile runLock(fixture.runPath + QStringLiteral(".lock"));
    QVERIFY(workerLock.tryLock(0));
    QVERIFY(runLock.tryLock(0));
    RecentBackupCopies copies(fixture.provider, fixture.configPath, QStringLiteral("computer"));
    QSignalSpy opened(&copies, &RecentBackupCopies::folderResolved);
    QSignalSpy failed(&copies, &RecentBackupCopies::failed);
    copies.openCopy(fixture.set.id);
    QTRY_COMPARE(opened.count(), 1);
    QCOMPARE(opened.first().first().toString(), fixture.path(QStringLiteral("recorded-copy")));
    QVERIFY(failed.isEmpty());
    QVERIFY(fixture.provider.downloaded.isEmpty());
    QVERIFY(fixture.provider.listed.isEmpty());
    workerLock.unlock();
    runLock.unlock();
    copies.requestDelete(fixture.set.id);
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(fixture.provider.downloaded.size(), 1);
    QVERIFY(fixture.provider.trashed.isEmpty());
}

void RecentBackupCopiesTest::browsingRejectsUnsafeRecordedPath()
{
    CopyFixture fixture;
    QVERIFY(fixture.prepare());
    BackupRunStore runs(fixture.runPath);
    QVERIFY(runs.load());
    runs.find(fixture.set.id)->remoteCopyPath = fixture.path(QStringLiteral("../other-copy"));
    QVERIFY(runs.save());
    RecentBackupCopies copies(fixture.provider, fixture.configPath, QStringLiteral("computer"));
    QSignalSpy opened(&copies, &RecentBackupCopies::folderResolved);
    QSignalSpy failed(&copies, &RecentBackupCopies::failed);
    copies.openCopy(fixture.set.id);
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(opened.isEmpty());
    QVERIFY(fixture.provider.downloaded.isEmpty());
    QVERIFY(fixture.provider.listed.isEmpty());
}

void RecentBackupCopiesTest::changedRunPointerPreventsDeletion()
{
    CopyFixture fixture;
    QVERIFY(fixture.prepare());
    QVERIFY(fixture.copy(QStringLiteral("original"), 0));
    RecentBackupCopies copies(fixture.provider, fixture.configPath, QStringLiteral("computer"));
    QSignalSpy warning(&copies, &RecentBackupCopies::deleteConfirmationReady);
    QSignalSpy failure(&copies, &RecentBackupCopies::failed);
    copies.requestDelete(fixture.set.id);
    QTRY_COMPARE(warning.count(), 1);
    BackupRunStore runs(fixture.runPath);
    QVERIFY(runs.load());
    runs.find(fixture.set.id)->remoteCopyPath = fixture.path(QStringLiteral("replacement"));
    QVERIFY(runs.save());
    copies.confirmDelete();
    QCOMPARE(copies.deletingSetId(), fixture.set.id);
    QTRY_COMPARE(failure.count(), 1);
    QVERIFY(copies.deletingSetId().isEmpty());
    QVERIFY(failure.first().first().toString().contains(QStringLiteral("changed")));
    QVERIFY(fixture.provider.trashed.isEmpty());
}

void RecentBackupCopiesTest::foreignManifestsAndUnsafePathsCannotBeDeleted()
{
    CopyFixture fixture;
    QVERIFY(fixture.prepare());
    QVERIFY(fixture.copy(QStringLiteral("foreign"), 0, QStringLiteral("another-set")));
    RecentBackupCopies copies(fixture.provider, fixture.configPath, QStringLiteral("computer"));
    QSignalSpy failure(&copies, &RecentBackupCopies::failed);
    QSignalSpy warning(&copies, &RecentBackupCopies::deleteConfirmationReady);
    copies.requestDelete(fixture.set.id);
    QTRY_COMPARE(failure.count(), 1);
    QVERIFY(warning.isEmpty());
    BackupRunStore runs(fixture.runPath);
    QVERIFY(runs.load());
    runs.find(fixture.set.id)->remoteCopyPath = QStringLiteral("backups/other-computer/other-set/copy");
    QVERIFY(runs.save());
    copies.requestDelete(fixture.set.id);
    QTRY_COMPARE(failure.count(), 2);
    QVERIFY(failure.last().first().toString().contains(QStringLiteral("outside")));
    QVERIFY(fixture.provider.trashed.isEmpty());
}

void RecentBackupCopiesTest::runningWorkerPreventsDeletion()
{
    CopyFixture fixture;
    QVERIFY(fixture.prepare());
    QLockFile lock(fixture.configPath + QStringLiteral(".worker.lock"));
    QVERIFY(lock.tryLock(0));
    RecentBackupCopies copies(fixture.provider, fixture.configPath, QStringLiteral("computer"));
    QSignalSpy failure(&copies, &RecentBackupCopies::failed);
    copies.requestDelete(fixture.set.id);
    QTRY_COMPARE(failure.count(), 1);
    QVERIFY(fixture.provider.trashed.isEmpty());
}

QTEST_GUILESS_MAIN(RecentBackupCopiesTest)
#include "recentbackupcopies_test.moc"

#include "settingsarchive.h"
#include "backupsetcontroller.h"
#include "backuprestorecontroller.h"
#include "incrementalworker.h"
#include "protonprovider.h"
#include "protonclifixture.h"
#include <QSignalSpy>
#include <QTest>

namespace {
QByteArray bytes(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
bool write(const QString &path, const QByteArray &data) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}
BackupConfig definitions(const QString &source) {
    BackupConfig config;
    BackupSet set{"documents", "Documents", "/my-files/backups", {source}, {"*.tmp"}};
    set.inclusions = {".env"};
    set.schedule = {"weekly", 15, 42, 4, 23};
    set.retention = 7;
    set.onlyOnAcPower = true;
    config.sets = {set};
    return config;
}
class ValidMismatchRunner final : public ProcessRunner {
public:
    FilesystemRunner fixture;
    bool mismatch = false;
    bool failUploads = false;
    ProcessOutput run(const QStringList &arguments) override {
        if (failUploads && arguments.value(1) == "upload") return {1, {}, "Injected upload failure"};
        auto result = fixture.run(arguments);
        if (mismatch && result.successful() && arguments.value(1) == "download") {
            const auto path = QDir(arguments.last()).filePath(QFileInfo(arguments[arguments.size() - 2]).fileName());
            auto contents = bytes(path);
            contents.replace("Documents", "TamperedDocs");
            if (!write(path, contents)) return {1, {}, "Fixture write failed"};
        }
        return result;
    }
};
}

class SettingsArchiveTest final : public QObject {
    Q_OBJECT
private slots:
    void validJsonVerificationMismatchPreservesPreviousVersions() {
        ValidMismatchRunner runner;
        QVERIFY(QDir().mkpath(runner.fixture.remoteFile("/my-files")));
        ProtonProvider provider(runner);
        QString error, first, failed;
        QVERIFY2(SettingsArchive::exportSettings(provider, definitions("/old/source"), "laptop", &first, &error), qPrintable(error));
        const auto original = bytes(runner.fixture.remoteFile(first));
        runner.mismatch = true;
        QVERIFY(!SettingsArchive::exportSettings(provider, definitions("/old/source"), "laptop", &failed, &error));
        QVERIFY2(error.contains("verification mismatch"), qPrintable(error));
        QVERIFY(failed.isEmpty());
        QCOMPARE(bytes(runner.fixture.remoteFile(first)), original);
        QVERIFY(runner.fixture.trashedPaths.isEmpty());
        runner.mismatch = false;
        runner.failUploads = true;
        QVERIFY(!SettingsArchive::exportSettings(provider, definitions("/old/source"), "laptop", &failed, &error));
        QCOMPARE(bytes(runner.fixture.remoteFile(first)), original);
    }
    void restoreWithoutOldSourcesOrHistory_data() {
        QTest::addColumn<bool>("incremental");
        QTest::addColumn<bool>("legacy");
        QTest::newRow("incremental") << true << false;
        QTest::newRow("legacy-full-copy") << false << true;
        QTest::newRow("mixed-history") << true << true;
    }
    void restoreWithoutOldSourcesOrHistory() {
        QFETCH(bool, incremental);
        QFETCH(bool, legacy);
        QTemporaryDir local(QDir::current().filePath("settings-recovery-XXXXXX"));
        FilesystemRunner runner;
        QVERIFY(QDir().mkpath(runner.remoteFile("/my-files")));
        ProtonProvider provider(runner);
        auto config = definitions(local.filePath("old-home/Documents"));
        config.sets[0].inclusions.clear();
        config.sets[0].stagingDirectory = local.filePath("stage");
        auto &set = config.sets[0];
        QVERIFY(write(set.sourceDirectories.first() + "/important.txt", "recover me"));
        BackupEngine engine;
        QString error;
        if (incremental) {
            IncrementalWriteRequest request;
            QVERIFY2(IncrementalWorker::request(set, local.filePath("old-state"), QUuid::createUuid().toString(QUuid::WithoutBraces), provider, &request, &error), qPrintable(error));
            const auto result = IncrementalWorker::run(set, request, provider);
            QVERIFY2(result.success(), result.issues.isEmpty() ? "backup failed" : qPrintable(result.issues.first().reason));
        }
        if (legacy) {
            const QString copy = "20261025T120000Z";
            BackupOptions options;
            options.freshCopy = true;
            options.retainLocalManifest = false;
            QString manifest;
            const BackupCopyMetadata metadata{"old-computer", set.id, set.name, copy, QDateTime::currentDateTimeUtc()};
            QVERIFY2(engine.backup(set.sourceDirectories, set.remoteFolder("old-computer") + '/' + copy,
                set.exclusions, metadata, provider, &manifest, &error, {}, nullptr, options), qPrintable(error));
        }
        QString version;
        QVERIFY2(SettingsArchive::exportSettings(provider, config, "old-computer", &version, &error), qPrintable(error));
        // Remove source accessibility without using any old configuration/run state.
        QVERIFY(QDir().rename(set.sourceDirectories.first(), local.filePath("unavailable-old-source")));
        const QString downloaded = local.filePath("recovery.json");
        QVERIFY2(SettingsArchive::download(provider, version, downloaded, &error), qPrintable(error));
        const QString fresh = local.filePath("replacement/settings.json");
        BackupSetController settings(engine, fresh);
        QVERIFY(settings.importSets(downloaded, false));
        QVERIFY(!QFile::exists(local.filePath("replacement/omacustos-backup-runs.json")));
        if (incremental) {
            const QString remoteRoot = IncrementalWorker::root(set);
            const auto anchor = bytes(runner.remoteFile(remoteRoot + "/repository.json"));
            const auto protectedOperation = remoteRoot + "/operations/" + QUuid::createUuid().toString(QUuid::WithoutBraces);
            QVERIFY(QDir().mkpath(runner.remoteFile(protectedOperation)));
            QVERIFY(!IncrementalWorker::takeover(set, local.filePath("replacement"), provider, true, &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(bytes(runner.remoteFile(remoteRoot + "/repository.json")), anchor);
            QVERIFY(QFileInfo::exists(runner.remoteFile(protectedOperation)));
        }
        BackupRestoreController restore(engine, &provider);
        restore.discover(settings.historyFolderPath(set.id), set.id, true, settings.historyLegacyName(set.id), true, true);
        QTRY_VERIFY_WITH_TIMEOUT(!restore.busy(), 30000);
        QCOMPARE(restore.copies().size(), int(incremental) + int(legacy));
        for (int i = 0; i < restore.copies().size(); ++i) {
            restore.selectCopy(i);
            QTRY_VERIFY_WITH_TIMEOUT(!restore.busy(), 30000);
            QVERIFY2(restore.restoreEligible(), qPrintable(restore.browseError()));
            QCOMPARE(restore.entries().size(), 1);
            const auto destination = local.filePath("restored-" + QString::number(i));
            restore.restore(0, destination);
            QTRY_VERIFY_WITH_TIMEOUT(!restore.busy(), 30000);
            QCOMPARE(restore.restoreState(), QString("succeeded"));
            QCOMPARE(bytes(destination + "/important.txt"), QByteArray("recover me"));
        }
        BackupConfig imported;
        QVERIFY(BackupConfigStore(fresh).load(&imported, &error));
        QVERIFY(imported.sets.first().readOnly && imported.sets.first().schedule.paused);
    }

    void roundTripThroughProtonWithNoOriginalState() {
        QTemporaryDir local;
        FilesystemRunner runner;
        QVERIFY(QDir().mkpath(runner.remoteFile("/my-files")));
        ProtonProvider provider(runner);
        auto config = definitions(local.filePath("missing-old-home/Documents"));
        QString error, first, second, other;
        QVERIFY2(SettingsArchive::exportSettings(provider, config, "old-laptop", &first, &error), qPrintable(error));
        QVERIFY2(SettingsArchive::exportSettings(provider, config, "old-laptop", &second, &error), qPrintable(error));
        QVERIFY2(SettingsArchive::exportSettings(provider, config, "desktop", &other, &error), qPrintable(error));
        QVERIFY(first != second && first != other);
        const auto original = bytes(runner.remoteFile(first));
        QVERIFY(!original.contains("installation_binding"));
        QVERIFY(!original.contains("read_only"));
        QVERIFY(!original.contains("proton_binary"));
        QVariantList versions;
        QVERIFY2(SettingsArchive::browse(provider, &versions, &error), qPrintable(error));
        QCOMPARE(versions.size(), 3);
        const auto downloaded = local.filePath("import.json");
        QVERIFY2(SettingsArchive::download(provider, first, downloaded, &error), qPrintable(error));
        BackupEngine engine;
        const auto path = local.filePath("replacement/settings.json");
        BackupSetController controller(engine, path);
        QVERIFY(controller.prepareImport(downloaded));
        QVERIFY(controller.importPreview().contains("Missing source"));
        QVERIFY(!QFile::exists(path)); // preview is not an import
        QVERIFY(controller.applyImport(false));
        BackupConfig imported;
        BackupConfigStore store(path);
        QVERIFY(store.load(&imported, &error));
        const auto set = imported.sets.first();
        QCOMPARE(set.sourceDirectories, config.sets.first().sourceDirectories);
        QCOMPARE(set.exclusions, config.sets.first().exclusions);
        QCOMPARE(set.inclusions, config.sets.first().inclusions);
        QCOMPARE(set.remoteRoot, config.sets.first().remoteRoot);
        QCOMPARE(set.retention, 7);
        QCOMPARE(set.schedule.frequency, QString("weekly"));
        QCOMPARE(set.schedule.hour, 15);
        QCOMPARE(set.schedule.minute, 42);
        QCOMPARE(set.schedule.weekday, 4);
        QCOMPARE(set.schedule.dayOfMonth, 23);
        QVERIFY(set.schedule.paused && !set.schedule.enabled() && set.readOnly);
        QVERIFY(!controller.resumeImportedSet(set.id));
        QVERIFY(store.grantWriteAuthority(set.id, set.remoteRoot, &error));
        QVERIFY(store.load(&imported, &error));
        QVERIFY(imported.sets.first().schedule.paused); // takeover never resumes
        QVERIFY(!controller.resumeImportedSet(set.id)); // missing sources still block
        QVERIFY(QDir().mkpath(set.sourceDirectories.first()));
        QVERIFY(controller.resumeImportedSet(set.id));
        QVERIFY(store.load(&imported, &error));
        QVERIFY(imported.sets.first().schedule.enabled());
        QVERIFY(!imported.sets.first().readOnly);
        QVERIFY(!QFile::exists(local.filePath("replacement/omacustos-backup-runs.json")));
        QCOMPARE(bytes(runner.remoteFile(first)), original);
    }

    void failuresPreserveEarlierExports() {
        QTemporaryDir local;
        FilesystemRunner runner;
        QVERIFY(QDir().mkpath(runner.remoteFile("/my-files")));
        ProtonProvider provider(runner);
        QString error, first, failed;
        const auto config = definitions("/missing/Documents");
        QVERIFY2(SettingsArchive::exportSettings(provider, config, "laptop", &first, &error), qPrintable(error));
        const auto original = bytes(runner.remoteFile(first));
        runner.downloadFailure = FilesystemRunner::Failure::CorruptOutput;
        QVERIFY(!SettingsArchive::exportSettings(provider, config, "laptop", &failed, &error));
        QVERIFY(failed.isEmpty());
        QCOMPARE(bytes(runner.remoteFile(first)), original);
        runner.downloadFailure = FilesystemRunner::Failure::TransferError;
        QVERIFY(!SettingsArchive::exportSettings(provider, config, "desktop", &failed, &error));
        QCOMPARE(bytes(runner.remoteFile(first)), original);
        QVERIFY(runner.trashedPaths.isEmpty());
    }

    void untrustedImportsLeaveSettingsUnchanged() {
        QTemporaryDir local;
        FilesystemRunner runner;
        QVERIFY(QDir().mkpath(runner.remoteFile("/my-files")));
        ProtonProvider provider(runner);
        QString error, remote;
        auto config = definitions("/missing/source");
        QVERIFY2(SettingsArchive::exportSettings(provider, config, "laptop", &remote, &error), qPrintable(error));
        BackupEngine engine;
        const QString path = local.filePath("settings.json"), download = local.filePath("download.json");
        QVERIFY(BackupConfigStore(path).save(config, &error));
        const auto before = bytes(path);
        BackupSetController controller(engine, path);
        QVERIFY(!SettingsArchive::download(provider, SettingsArchive::root() + "/laptop/../../outside.json", download, &error));
        QVERIFY(write(runner.remoteFile(remote), QByteArray(BackupConfigStore::MaximumImportBytes + 1, ' ')));
        QVERIFY(!SettingsArchive::download(provider, remote, download, &error));
        QVERIFY(write(runner.remoteFile(remote), "{invalid"));
        QVERIFY(!SettingsArchive::download(provider, remote, download, &error));
        QVERIFY(!controller.prepareImport(runner.remoteFile(remote)));
        QVERIFY(!controller.applyImport(false));
        QCOMPARE(bytes(path), before);
        QVERIFY(write(runner.remoteFile(remote), R"({"application":"omacustos","version":1,"sets":[{"id":"../escape","name":"Unsafe","remote_root":"/backups","source_directories":["/missing"]}]})"));
        QVERIFY(!controller.importSets(runner.remoteFile(remote), true));
        QCOMPARE(bytes(path), before);
        runner.wrongListedParentName = QFileInfo(remote).fileName();
        QVariantList versions;
        QVERIFY(!SettingsArchive::browse(provider, &versions, &error));
        QCOMPARE(bytes(path), before);
    }

    void localMergeAndReplaceRemainPausedAcrossRestart() {
        QTemporaryDir local;
        auto original = definitions("/old/source");
        auto incoming = original;
        incoming.sets[0].name = "Updated";
        auto sameName = original.sets.first(); sameName.id = "another-id";
        incoming.sets.append(sameName);
        auto unrelated = original.sets.first(); unrelated.id = "unrelated";
        original.sets.append(unrelated);
        QString error;
        const auto path = local.filePath("settings.json"), file = local.filePath("import.json");
        QVERIFY(BackupConfigStore(path).save(original, &error));
        QVERIFY(BackupConfigStore(file).exportSets(incoming, &error));
        BackupEngine engine;
        {
            BackupSetController controller(engine, path);
            QVERIFY(controller.importSets(file, true));
        }
        BackupConfig merged;
        QVERIFY(BackupConfigStore(path).load(&merged, &error));
        QCOMPARE(merged.sets.size(), 3);
        QCOMPARE(merged.sets[0].name, QString("Updated"));
        QVERIFY(merged.sets[0].readOnly && merged.sets[0].schedule.paused);
        QVERIFY(!merged.sets[1].schedule.paused); // unrelated definition preserved
        QVERIFY(merged.sets[2].readOnly && merged.sets[2].schedule.paused);
        BackupSetController reopened(engine, path);
        QVERIFY(reopened.importSets(file, false));
        QVERIFY(BackupConfigStore(path).load(&merged, &error));
        QCOMPARE(merged.sets.size(), 2);
        for (const auto &set : merged.sets) QVERIFY(set.readOnly && set.schedule.paused);
    }
};
QTEST_GUILESS_MAIN(SettingsArchiveTest)
#include "settingsarchive_test.moc"

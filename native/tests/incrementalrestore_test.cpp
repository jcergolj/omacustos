#include "incrementalrestore.h"
#include "incrementalrepository.h"
#include "incrementalmanifest.h"
#include "backuparchive.h"
#include "localprovider.h"
#include "protonprovider.h"
#include "protonclifixture.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>

namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QJsonObject object(const QString &path) { return QJsonDocument::fromJson(read(path)).object(); }
QByteArray hash(const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }

struct Fixture {
    QTemporaryDir directory{QDir::current().filePath("incremental-restore-fixture-XXXXXX")};
    LocalProvider provider{directory.filePath("cloud")};
    IncrementalWriteRequest request;
    QString error;
    Fixture()
    {
        QDir().mkpath(directory.filePath("cloud/backup"));
        QDir().mkpath(directory.filePath("source"));
        request.scan.setId = uuid();
        request.remoteSetRoot = "backup/sets/" + request.scan.setId;
        request.computerId = uuid();
        request.originComputerId = request.computerId;
        request.stateDirectory = directory.filePath("state");
        request.stagingDirectory = directory.filePath("staging");
        next();
    }
    void next()
    {
        request.operationId = uuid(); request.snapshotId = uuid();
        request.scan.checkedAt = QDateTime::currentDateTimeUtc();
    }
    bool source(const QString &name, const QByteArray &bytes)
    {
        const QString path = directory.filePath("source/" + name);
        if (!write(path, bytes)) return false;
        for (const auto &source : request.scan.sources) if (source.sourcePath == path) return true;
        request.scan.sources.append({path, name});
        return true;
    }
    bool initialize() { return IncrementalRepository::initialize(request, provider, true, &error); }
    bool save() { return IncrementalRepository::execute(request, provider).success(); }
    QString snapshot() const { return request.remoteSetRoot + "/snapshots/" + request.snapshotId; }
    QString cloud(const QString &remote) const { return directory.filePath("cloud/" + remote); }
    bool load(RemoteCopy *copy) { return BackupCatalog::verifyCopy(provider, snapshot(), request.scan.setId, copy, &error); }
};
}

class IncrementalRestoreTest : public QObject {
    Q_OBJECT
private slots:
    void sharedArchiveVerificationUsesBulkMetadata_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<int>("calls");
        QTest::addColumn<int>("available");
        QTest::newRow("complete metadata") << QString("bulk") << 2 << 3;
        QTest::newRow("partial metadata") << QString("partial") << 3 << 3;
        QTest::newRow("storage-only metadata") << QString("storage") << 5 << 3;
        QTest::newRow("failed listing") << QString("failed") << 5 << 3;
        QTest::newRow("missing archive") << QString("missing") << 3 << 2;
        QTest::newRow("changed archive size") << QString("size") << 2 << 2;
    }

    void sharedArchiveVerificationUsesBulkMetadata()
    {
        QFETCH(QString, scenario);
        QFETCH(int, calls);
        QFETCH(int, available);
        Fixture f;
        QVERIFY(f.source("a", "alpha"));
        QVERIFY(f.source("b", "beta"));
        QVERIFY(f.source("c", "gamma"));
        FilesystemRunner runner;
        ProtonProvider provider(runner);
        QVERIFY(QDir().mkpath(runner.remoteFile("/my-files")));
        f.request.remoteSetRoot = "/my-files/sets/" + f.request.scan.setId;
        f.request.archiveTargetBytes = 1;
        QString error;
        QVERIFY(IncrementalRepository::initialize(f.request, provider, true, &error));
        QVERIFY(IncrementalRepository::execute(f.request, provider).success());
        const auto manifest = object(runner.remoteFile(f.snapshot() + "/manifest.json"));
        const auto archives = manifest.value("archives").toArray();
        QCOMPARE(archives.size(), 3);
        const auto remote = f.request.remoteSetRoot + '/' + archives.first().toObject().value("remote").toString();
        runner.includeListingContentSize = scenario != "storage";
        if (scenario == "partial") runner.omitListingMetadataName = QFileInfo(remote).fileName();
        if (scenario == "failed") runner.failListPath = f.request.remoteSetRoot + "/objects";
        if (scenario == "missing") QVERIFY(QFile::remove(runner.remoteFile(remote)));
        if (scenario == "size") {
            QVERIFY(QFile::setPermissions(runner.remoteFile(remote), QFileDevice::ReadOwner | QFileDevice::WriteOwner));
            QVERIFY(write(runner.remoteFile(remote), "changed size"));
        }
        runner.calls.clear();
        RemoteCopy copy;
        QVERIFY2(IncrementalRestore::verifyCopy(provider, f.snapshot(), f.request.scan.setId, &copy, &error), qPrintable(error));
        QCOMPARE(copy.entries.size(), available);
        QCOMPARE(copy.unavailableItems.size(), 3 - available);
        QCOMPARE(runner.calls.size(), calls);
        QCOMPARE(runner.calls.first().mid(7, 2), (QStringList {f.request.remoteSetRoot + "/repository.json", f.snapshot() + "/manifest.json"}));
        for (const auto &call : runner.calls) {
            QVERIFY(call.value(1) == "download" || call.value(1) == "list" || call.value(1) == "info");
            if (call.value(1) == "download") QVERIFY(call.at(call.size() - 2).endsWith(".json"));
        }
        runner.calls.clear();
        RemoteCopy cancelled;
        QVERIFY(!IncrementalRestore::verifyCopy(provider, f.snapshot(), f.request.scan.setId, &cancelled, &error,
            [&] { return !runner.calls.isEmpty(); }));
        QCOMPARE(runner.calls.size(), 1); // Cancellation after the batch prevents archive inspection/publication.
        QVERIFY(cancelled.entries.isEmpty());
    }

    void selectedHistoryRejectsInvalidRepository_data()
    {
        QTest::addColumn<QString>("damage");
        for (const QString &damage : {"missing", "malformed", "set", "origin", "epoch"})
            QTest::newRow(qPrintable(damage)) << damage;
    }

    void selectedHistoryRejectsInvalidRepository()
    {
        QFETCH(QString, damage);
        Fixture f;
        QVERIFY(f.source("a", "alpha")); QVERIFY(f.initialize()); QVERIFY(f.save());
        const QString path = f.cloud(f.request.remoteSetRoot + "/repository.json");
        auto control = object(path);
        if (damage == "missing") QVERIFY(QFile::remove(path));
        else if (damage == "malformed") QVERIFY(write(path, "not JSON"));
        else {
            if (damage == "set") control["set_id"] = uuid();
            if (damage == "origin") control["origin_computer_id"] = "invalid";
            if (damage == "epoch") control["ownership_epoch"] = 1.5;
            QVERIFY(write(path, QJsonDocument(control).toJson()));
        }
        QVector<RemoteCopy> copies;
        QVERIFY(!BackupCatalog::listHistory(f.provider, "backup", &copies, &f.error, {}, f.request.scan.setId));
        QVERIFY(copies.isEmpty());
        QVERIFY(!f.error.isEmpty());
        QVERIFY(IncrementalRestore::listSnapshotCandidates(f.provider, "backup", f.request.scan.setId, &copies, &f.error));
        QCOMPARE(copies.size(), 1);
        QVERIFY(!copies.first().complete());
        QVERIFY(copies.first().entries.isEmpty());
    }

    void selectedHistoryKeepsPublishedCopiesAndRefreshesIdentity()
    {
        Fixture f;
        QVERIFY(f.source("a", "alpha")); QVERIFY(f.initialize()); QVERIFY(f.save());
        const QString first = f.snapshot();
        f.next(); QVERIFY(f.source("a", "changed")); QVERIFY(f.save());
        QVERIFY(QFile::remove(f.cloud(first + "/manifest.json")));
        QVector<RemoteCopy> copies;
        QVERIFY(BackupCatalog::listHistory(f.provider, "backup", &copies, &f.error, {}, f.request.scan.setId));
        QCOMPARE(copies.size(), 1);
        QCOMPARE(copies.first().rootPath, f.snapshot());
        QVERIFY(copies.first().entries.isEmpty());
        QVERIFY(!f.error.isEmpty()); // An unpublished/missing manifest is not a restore point.
        QVector<RemoteCopy> candidates;
        QVERIFY(IncrementalRestore::listSnapshotCandidates(f.provider, "backup", f.request.scan.setId, &candidates, &f.error));
        QCOMPARE(candidates.size(), 2); // Display-only folders may include an unpublished point.
        for (const auto &candidate : candidates) {
            QCOMPARE(candidate.status, QString("not verified"));
            QVERIFY(candidate.entries.isEmpty());
        }

        const QString path = f.cloud(f.request.remoteSetRoot + "/repository.json");
        auto control = object(path);
        control["origin_computer_id"] = uuid();
        QVERIFY(write(path, QJsonDocument(control).toJson()));
        f.error.clear();
        QVERIFY(BackupCatalog::listHistory(f.provider, "backup", &copies, &f.error, {}, f.request.scan.setId));
        QVERIFY(copies.isEmpty()); // A previous pass's identity must never authorize a later one.
        QVERIFY(!f.error.isEmpty());
    }

    void selectedHistoryRejectsUnexpectedSnapshotEntries_data()
    {
        QTest::addColumn<bool>("directory");
        QTest::newRow("invalid directory name") << true;
        QTest::newRow("file in snapshot namespace") << false;
    }

    void selectedHistoryRejectsUnexpectedSnapshotEntries()
    {
        QFETCH(bool, directory);
        Fixture f;
        QVERIFY(f.source("a", "alpha")); QVERIFY(f.initialize()); QVERIFY(f.save());
        const QString path = f.cloud(f.request.remoteSetRoot + "/snapshots/" + (directory ? "unexpected" : uuid()));
        if (directory) QVERIFY(QDir().mkpath(path));
        else QVERIFY(write(path, "not a snapshot"));
        QVector<RemoteCopy> copies;
        QVERIFY(!BackupCatalog::listHistory(f.provider, "backup", &copies, &f.error, {}, f.request.scan.setId));
        QVERIFY(copies.isEmpty());
        QVERIFY(!f.error.isEmpty());
        QVERIFY(!IncrementalRestore::listSnapshotCandidates(f.provider, "backup", f.request.scan.setId, &copies, &f.error));
        QVERIFY(copies.isEmpty());
    }

    void selectedHistoryRejectsUnsafeRootsAndHonorsCancellation()
    {
        Fixture f;
        QVERIFY(f.source("a", "alpha")); QVERIFY(f.initialize()); QVERIFY(f.save());
        QVector<RemoteCopy> copies;
        for (const QString &root : {"backup/../backup", "backup//", "backup\\other", ""}) {
            QVERIFY(!BackupCatalog::listHistory(f.provider, root, &copies, &f.error, {}, f.request.scan.setId));
            QVERIFY(copies.isEmpty());
            QVERIFY(!IncrementalRestore::listSnapshotCandidates(f.provider, root, f.request.scan.setId, &copies, &f.error));
            QVERIFY(copies.isEmpty());
        }
        QVERIFY(!BackupCatalog::listHistory(f.provider, "backup", &copies, &f.error, [] { return true; }, f.request.scan.setId));
        QVERIFY(copies.isEmpty());
        QVERIFY(!IncrementalRestore::listSnapshotCandidates(f.provider, "backup", f.request.scan.setId,
            &copies, &f.error, [] { return true; }));
        QVERIFY(copies.isEmpty());
        int polls = 0;
        QVERIFY(!IncrementalRestore::listSnapshotCandidates(f.provider, "backup", f.request.scan.setId,
            &copies, &f.error, [&] { return ++polls > 3; }));
        QVERIFY(copies.isEmpty());
    }

    void legacyComputerNamedSetsStillDiscovered()
    {
        Fixture f;
        QVERIFY(f.source("a", "legacy"));
        BackupEngine engine;
        const QString root = "backup/sets/Documents/legacy-copy";
        BackupCopyMetadata metadata {"sets", "Documents", "Documents", "legacy-copy", QDateTime::currentDateTimeUtc()};
        QString manifest;
        QVERIFY2(engine.backup(QStringList{f.directory.filePath("source")}, root, {}, metadata,
            f.provider, &manifest, &f.error), qPrintable(f.error));
        QVector<RemoteCopy> copies;
        QVERIFY2(BackupCatalog::discover(f.provider, "backup", &copies, &f.error), qPrintable(f.error));
        QCOMPARE(copies.size(), 1);
        QCOMPARE(copies[0].computerName, QString("sets"));
        QCOMPARE(copies[0].entries.size(), 1);
        const auto restored = engine.restoreFiles({copies[0].entries, f.directory.filePath("restored"), root}, f.provider);
        QVERIFY2(restored.success, qPrintable(restored.error));
    }

    void generationsWithoutAncestorsOrLocalConfiguration()
    {
        Fixture f;
        QVERIFY(f.source("a", "alpha")); QVERIFY(f.source("b", "beta")); QVERIFY(f.source("c", "gamma"));
        QVERIFY2(f.initialize(), qPrintable(f.error)); QVERIFY(f.save());
        const QString old = f.snapshot();
        RemoteCopy original;
        QVERIFY(f.load(&original));
        f.next();
        QVERIFY(f.source("a", "edited"));
        f.request.scan.sources.removeAt(1); // Delete b logically; its physical bytes remain in the archive.
        f.request.scan.sources[1].restorePath = "moved/renamed-c";
        QVERIFY(f.save());
        RemoteCopy current;
        QVERIFY2(f.load(&current), qPrintable(f.error));
        QCOMPARE(current.entries.size(), 2);
        QSet<QString> generations;
        for (const auto &entry : current.entries) generations.insert(entry.archive.id);
        QCOMPARE(generations.size(), 2);
        BackupEngine engine;
        auto restored = engine.restoreFiles({original.entries, f.directory.filePath("old"), old}, f.provider);
        QVERIFY2(restored.success, qPrintable(restored.error));
        QCOMPARE(read(f.directory.filePath("old/a")), QByteArray("alpha"));
        QCOMPARE(read(f.directory.filePath("old/b")), QByteArray("beta"));
        QVERIFY(QFile::remove(f.cloud(old + "/manifest.json"))); // No ancestor may be needed now.
        QVERIFY(QDir(f.request.stateDirectory).removeRecursively()); // Fixture-owned local journals only.
        QVERIFY(QDir(f.directory.filePath("source")).removeRecursively());
        QVector<RemoteCopy> discovered;
        QVERIFY2(IncrementalRestore::discover(f.provider, "backup", &discovered, &f.error), qPrintable(f.error));
        QCOMPARE(discovered.size(), 1);
        restored = engine.restoreFiles({discovered[0].entries, f.directory.filePath("new"), discovered[0].rootPath}, f.provider);
        QVERIFY2(restored.success, qPrintable(restored.error));
        QCOMPARE(read(f.directory.filePath("new/a")), QByteArray("edited"));
        QCOMPARE(read(f.directory.filePath("new/moved/renamed-c")), QByteArray("gamma"));
        QVERIFY(!QFileInfo::exists(f.directory.filePath("new/b")));
        QVERIFY(!QFileInfo::exists(f.directory.filePath("new/c")));
        QCOMPARE(QDir(f.directory.filePath("new")).entryList(QDir::Files), QStringList{"a"});
        QVector<RemoteCopy> catalog;
        QVERIFY2(BackupCatalog::discover(f.provider, "backup", &catalog, &f.error), qPrintable(f.error));
        QCOMPARE(catalog.size(), 1);
    }

    void selectiveRestoreAndDuplicateContentDestinations()
    {
        Fixture f;
        QVERIFY(f.source("a", "same")); QVERIFY(f.source("b", "other"));
        QVERIFY(f.initialize()); QVERIFY(f.save());
        auto manifest = object(f.cloud(f.snapshot() + "/manifest.json"));
        auto entries = manifest.value("entries").toArray();
        auto duplicate = entries[0].toObject(); duplicate["restore"] = "copy-of-a";
        entries.append(duplicate); manifest["entries"] = entries;
        QVERIFY(write(f.cloud(f.snapshot() + "/manifest.json"), QJsonDocument(manifest).toJson()));
        RemoteCopy copy; QVERIFY2(f.load(&copy), qPrintable(f.error));
        BackupEngine engine;
        const auto one = engine.restoreFiles({{copy.entries[0]}, f.directory.filePath("one"), copy.rootPath}, f.provider);
        QVERIFY2(one.success, qPrintable(one.error));
        QVERIFY(!QFileInfo::exists(f.directory.filePath("one/b")));
        const auto all = engine.restoreFiles({copy.entries, f.directory.filePath("all"), copy.rootPath}, f.provider);
        QVERIFY2(all.success, qPrintable(all.error));
        QCOMPARE(all.restoredCount, 3);
        QCOMPARE(read(f.directory.filePath("all/copy-of-a")), QByteArray("same"));
    }

    void logicalMetadataIsExact()
    {
        Fixture f;
        QVERIFY(f.source("a", "alpha")); QVERIFY(f.initialize()); QVERIFY(f.save());
        auto manifest = object(f.cloud(f.snapshot() + "/manifest.json"));
        auto entries = manifest.value("entries").toArray();
        auto entry = entries[0].toObject(); entry["mode"] = 0751; entry["mtime_ns"] = "1700000000123456789";
        entries[0] = entry; manifest["entries"] = entries;
        QVERIFY(write(f.cloud(f.snapshot() + "/manifest.json"), QJsonDocument(manifest).toJson()));
        RemoteCopy copy; QVERIFY(f.load(&copy));
        BackupEngine engine;
        const auto result = engine.restoreFiles({copy.entries, f.directory.filePath("restored"), copy.rootPath}, f.provider);
        QVERIFY2(result.success, qPrintable(result.error));
        struct stat metadata {};
        QCOMPARE(::stat(QFile::encodeName(f.directory.filePath("restored/a")).constData(), &metadata), 0);
        QCOMPARE(metadata.st_mode & 0777, 0751u);
        QCOMPARE(metadata.st_mtim.tv_sec, 1700000000L);
        QCOMPARE(metadata.st_mtim.tv_nsec, 123456789L);
    }

    void malformedAndForeignManifests_data()
    {
        QTest::addColumn<QString>("damage");
        for (const QString &damage : {"outside", "unsafe-destination", "unsafe-member", "origin", "set", "snapshot", "future-epoch", "inventory", "version"})
            QTest::newRow(qPrintable(damage)) << damage;
    }
    void malformedAndForeignManifests()
    {
        QFETCH(QString, damage);
        Fixture f; QVERIFY(f.source("a", "alpha")); QVERIFY(f.initialize()); QVERIFY(f.save());
        auto manifest = object(f.cloud(f.snapshot() + "/manifest.json"));
        auto archives = manifest.value("archives").toArray(); auto archive = archives[0].toObject();
        auto entries = manifest.value("entries").toArray(); auto entry = entries[0].toObject();
        if (damage == "outside") archive["remote"] = "../foreign.tar.gz";
        if (damage == "unsafe-destination") entry["restore"] = "../escape";
        if (damage == "unsafe-member") entry["member"] = "../escape";
        if (damage == "origin") manifest["origin_computer_id"] = uuid();
        if (damage == "set") manifest["set_id"] = uuid();
        if (damage == "snapshot") manifest["snapshot_id"] = uuid();
        if (damage == "future-epoch") manifest["ownership_epoch"] = 2;
        if (damage == "inventory") archive["members"] = QJsonArray();
        if (damage == "version") manifest["version"] = 5;
        archives[0] = archive; entries[0] = entry;
        manifest["archives"] = archives; manifest["entries"] = entries;
        QVERIFY(write(f.cloud(f.snapshot() + "/manifest.json"), QJsonDocument(manifest).toJson()));
        RemoteCopy copy; QVERIFY(!f.load(&copy));
        QVector<RemoteCopy> listed;
        f.error.clear();
        QVERIFY(BackupCatalog::listHistory(f.provider, "backup", &listed, &f.error, {}, f.request.scan.setId));
        QVERIFY(listed.isEmpty());
        QVERIFY(!f.error.isEmpty());
        QVector<RemoteCopy> discovered;
        QVERIFY(IncrementalRestore::discover(f.provider, "backup", &discovered, &f.error));
        QCOMPARE(discovered.size(), 1); QCOMPARE(discovered[0].status, QString("damaged"));
        QVERIFY(discovered[0].entries.isEmpty());
    }

    void changedOrMissingArchive_data()
    {
        QTest::addColumn<bool>("missing");
        QTest::newRow("missing") << true; QTest::newRow("corrupt") << false;
    }
    void changedOrMissingArchive()
    {
        QFETCH(bool, missing);
        Fixture f; QVERIFY(f.source("a", "alpha")); QVERIFY(f.initialize()); QVERIFY(f.save());
        RemoteCopy copy; QVERIFY(f.load(&copy));
        const auto archive = copy.entries[0].archive;
        if (missing) QVERIFY(QFile::remove(f.cloud(archive.remotePath)));
        else {
            QVERIFY(QFile::setPermissions(f.cloud(archive.remotePath), QFileDevice::ReadOwner | QFileDevice::WriteOwner));
            QVERIFY(write(f.cloud(archive.remotePath), QByteArray(archive.size, 'x')));
        }
        BackupEngine engine;
        const auto result = engine.restoreFiles({copy.entries, f.directory.filePath("restored"), copy.rootPath}, f.provider);
        QVERIFY(!result.success); QCOMPARE(result.restoredCount, 0);
        QVERIFY(!QFileInfo::exists(f.directory.filePath("restored/a")));
        QVERIFY(f.load(&copy)); QCOMPARE(copy.unavailableItems, QStringList{"a"}); QVERIFY(copy.entries.isEmpty());
    }

    void fullInventoryValidatedEvenForUnselectedBytes_data()
    {
        QTest::addColumn<QString>("damage");
        for (const QString &damage : {"none", "unselected-hash", "unselected-size", "extra", "duplicate", "unsafe", "symlink", "hardlink", "truncated", "trailer"})
            QTest::newRow(qPrintable(damage)) << damage;
    }
    void fullInventoryValidatedEvenForUnselectedBytes()
    {
        QFETCH(QString, damage);
        QTemporaryDir directory;
        const QString path = directory.filePath("archive.tar.gz");
        auto *writer = archive_write_new();
        QCOMPARE(archive_write_set_format_pax_restricted(writer), ARCHIVE_OK);
        QCOMPARE(archive_write_add_filter_gzip(writer), ARCHIVE_OK);
        QCOMPARE(archive_write_open_filename(writer, QFile::encodeName(path).constData()), ARCHIVE_OK);
        QStringList names{"a", "b"};
        if (damage == "extra") names.append("extra");
        if (damage == "duplicate") names.append("b");
        if (damage == "unsafe") names[1] = "../escape";
        for (const auto &name : names) {
            auto *header = archive_entry_new();
            archive_entry_set_pathname(header, name.toUtf8().constData());
            archive_entry_set_filetype(header, damage == "symlink" && name == "b" ? AE_IFLNK : AE_IFREG);
            if (damage == "symlink" && name == "b") archive_entry_set_symlink(header, "a");
            if (damage == "hardlink" && name == "b") archive_entry_set_hardlink(header, "a");
            const bool link = (damage == "symlink" || damage == "hardlink") && name == "b";
            archive_entry_set_size(header, link ? 0 : 1);
            QCOMPARE(archive_write_header(writer, header), ARCHIVE_OK);
            if (!link) QCOMPARE(archive_write_data(writer, "x", 1), la_ssize_t(1));
            archive_entry_free(header);
        }
        QCOMPARE(archive_write_close(writer), ARCHIVE_OK); archive_write_free(writer);
        if (damage == "truncated" || damage == "trailer") {
            auto bytes = read(path);
            if (damage == "truncated") bytes.chop(8); else bytes[bytes.size() - 8] ^= 1;
            QVERIFY(write(path, bytes));
        }
        QVector<BackupPhysicalMember> inventory{{"a", 1, hash("x")}, {"b", 1, hash("x")}};
        if (damage == "unselected-hash") inventory[1].checksum = hash("y");
        if (damage == "unselected-size") inventory[1].size = 2;
        BackupEntry entry; entry.restorePath = "renamed"; entry.memberPath = "a"; entry.size = 1; entry.checksum = hash("x");
        QVector<BackupEntry> payloads; QString error;
        QCOMPARE(BackupArchiveIO::extractShared(path, inventory, {entry}, directory.path(), &payloads, &error), damage == "none");
        if (damage == "none") { QCOMPARE(payloads.size(), 1); QCOMPARE(payloads[0].restorePath, QString("renamed")); }
        else QVERIFY(payloads.isEmpty());
        QVERIFY(!QFileInfo::exists(directory.filePath("b"))); // Unselected members never materialized.
    }

    void setContainmentAndDestinationSymlink()
    {
        Fixture f; QVERIFY(f.source("a", "alpha")); QVERIFY(f.initialize()); QVERIFY(f.save());
        RemoteCopy copy; QVERIFY(f.load(&copy)); BackupEngine engine;
        auto result = engine.restoreFiles({copy.entries, f.directory.filePath("restored"), {}}, f.provider);
        QVERIFY(!result.success);
        result = engine.restoreFiles({copy.entries, f.directory.filePath("restored"), "backup/sets/" + uuid() + "/snapshots/" + uuid()}, f.provider);
        QVERIFY(!result.success);
        QVERIFY(QDir().mkpath(f.directory.filePath("outside"))); QVERIFY(QDir().mkpath(f.directory.filePath("restored")));
        QVERIFY(QFile::link(f.directory.filePath("outside/a"), f.directory.filePath("restored/a")));
        result = engine.restoreFiles({copy.entries, f.directory.filePath("restored"), copy.rootPath}, f.provider);
        QVERIFY(!result.success); QVERIFY(!QFileInfo::exists(f.directory.filePath("outside/a")));
    }

    void offlineTakeoverIsExplicitAndOldIdentityCannotWrite()
    {
        Fixture f; QVERIFY(f.source("a", "alpha")); QVERIFY(f.initialize()); QVERIFY(f.save());
        const QString newOwner = uuid();
        QVERIFY(!IncrementalRepository::takeover(f.request, f.provider, newOwner, false, &f.error));
        auto wrong = f.request; ++wrong.ownershipEpoch;
        QVERIFY(!IncrementalRepository::takeover(wrong, f.provider, newOwner, true, &f.error));
        QVERIFY2(IncrementalRepository::takeover(f.request, f.provider, newOwner, true, &f.error), qPrintable(f.error));
        RemoteCopy copy; QVERIFY2(f.load(&copy), qPrintable(f.error)); // Earlier owner remains readable.
        f.next(); QVERIFY(!f.save());
        f.request.computerId = newOwner; ++f.request.ownershipEpoch;
        QVERIFY(f.save());
        const auto anchor = object(f.cloud(f.request.remoteSetRoot + "/repository.json"));
        QCOMPARE(anchor.value("origin_computer_id").toString(), f.request.originComputerId);
        QCOMPARE(anchor.value("ownership_epoch").toInteger(), 2);
    }

    void unfinishedOperationsBlockTakeover()
    {
        Fixture f; QVERIFY(f.source("a", "alpha")); QVERIFY(f.initialize());
        f.request.checkpoint = [](const QString &phase) { return phase == "protected" ? IncrementalControl::Pause : IncrementalControl::Continue; };
        const auto result = IncrementalRepository::execute(f.request, f.provider);
        QCOMPARE(result.state, IncrementalWriteResult::State::Paused);
        QVERIFY(!IncrementalRepository::takeover(f.request, f.provider, uuid(), true, &f.error));
    }

    void protonAdapterReadOnlyRecovery()
    {
        Fixture f; QVERIFY(f.source("a", "alpha"));
        FilesystemRunner runner; ProtonProvider provider(runner);
        QVERIFY(QDir().mkpath(runner.remoteFile("/my-files")));
        f.request.remoteSetRoot = "/my-files/sets/" + f.request.scan.setId;
        QVERIFY2(IncrementalRepository::initialize(f.request, provider, true, &f.error), qPrintable(f.error));
        QVERIFY(IncrementalRepository::execute(f.request, provider).success());
        runner.calls.clear();
        QVector<RemoteCopy> copies;
        QVERIFY2(IncrementalRestore::discover(provider, "/my-files", &copies, &f.error), qPrintable(f.error));
        QCOMPARE(copies.size(), 1);
        BackupEngine engine;
        const auto result = engine.restoreFiles({copies[0].entries, f.directory.filePath("restored"), copies[0].rootPath}, provider);
        QVERIFY2(result.success, qPrintable(result.error));
        for (const auto &call : runner.calls) {
            QVERIFY(!call.contains("upload")); QVERIFY(!call.contains("trash")); QVERIFY(!call.contains("mkdir"));
        }
        const QString replacement = uuid();
        QVERIFY(!IncrementalRepository::takeover(f.request, provider, replacement, false, &f.error));
        QVERIFY2(IncrementalRepository::takeover(f.request, provider, replacement, true, &f.error), qPrintable(f.error));
        RemoteCopy historical;
        QVERIFY2(IncrementalRestore::verifyCopy(provider, copies[0].rootPath, {}, &historical, &f.error), qPrintable(f.error));
        QCOMPARE(historical.entries.size(), 1);
    }
};

QTEST_GUILESS_MAIN(IncrementalRestoreTest)
#include "incrementalrestore_test.moc"

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

#include <QFile>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include "../src/backupmanifest.h"
#include "../src/backupengine.h"
#include "../src/localprovider.h"

class BackupManifestTest final : public QObject
{
    Q_OBJECT

private slots:
    void constructsReadableCopyState_data();
    void constructsReadableCopyState();
    void rejectsInvalidDraftWithoutReplacingManifest_data();
    void rejectsInvalidDraftWithoutReplacingManifest();
    void loadsVersionedEntries();
    void rejectsTraversalPaths();
    void acceptsDotsInsideFileNames();
    void acceptsAbsoluteRemotePaths();
    void rejectsCompleteCopyWithMissingExpectedEntry();
    void rejectsFailedEntryPresentedAsVerified();
    void rejectsMalformedVersionTwoManifests_data();
    void rejectsMalformedVersionTwoManifests();
    void rejectsMalformedEntries();
    void rejectsNullOutput();
    void restoresOnlyTheSelectedFile();
    void rejectsTamperedRestore();
    void preservesExistingDestinationWhenRestoreFails();
    void rejectsRestoreThroughDestinationSymlink();
};

namespace {

BackupManifestDraft completeDraft()
{
    BackupManifestDraft draft;
    draft.metadata = {QStringLiteral("computer"), QStringLiteral("set-id"), QStringLiteral("Documents"),
        QStringLiteral("copy"), QDateTime::fromString(QStringLiteral("2026-09-28T12:00:00.000Z"), Qt::ISODateWithMs)};
    draft.verifiedEntries = {{QStringLiteral("/source/notes.txt"), QStringLiteral("copy/notes.txt"), 5,
        QCryptographicHash::hash("notes", QCryptographicHash::Sha256), QStringLiteral("notes.txt")}};
    draft.expectedItems = {QStringLiteral("notes.txt")};
    return draft;
}

}

void BackupManifestTest::constructsReadableCopyState_data()
{
    QTest::addColumn<QString>("state");
    QTest::newRow("complete") << QStringLiteral("complete");
    QTest::newRow("mixed upload results") << QStringLiteral("failed");
    QTest::newRow("missing expected payload") << QStringLiteral("missing");
    QTest::newRow("transfer issue with all payloads verified") << QStringLiteral("issue");
    QTest::newRow("selection failure outside expected payloads") << QStringLiteral("selection");
    QTest::newRow("legacy copy") << QStringLiteral("legacy");
    QTest::newRow("large payload metadata") << QStringLiteral("large");
}

void BackupManifestTest::constructsReadableCopyState()
{
    QFETCH(QString, state);
    BackupManifestDraft draft = completeDraft();
    if (state == QStringLiteral("failed") || state == QStringLiteral("missing")) {
        draft.expectedItems.append(QStringLiteral("failed.txt"));
    }
    if (state == QStringLiteral("failed")) {
        draft.failedItems.append(QStringLiteral("failed.txt"));
        draft.issues.append({QStringLiteral("/source/failed.txt"), QStringLiteral("uploading"), QStringLiteral("Upload failed.")});
    } else if (state == QStringLiteral("issue")) {
        draft.issues.append({QStringLiteral("/source"), QStringLiteral("uploading"), QStringLiteral("Folder transfer failed.")});
    } else if (state == QStringLiteral("selection")) {
        draft.failedItems.append(QStringLiteral("/source/missing.txt"));
        draft.issues.append({QStringLiteral("/source/missing.txt"), QStringLiteral("selection"), QStringLiteral("Missing source.")});
    } else if (state == QStringLiteral("legacy")) {
        draft.metadata = {};
        draft.verifiedEntries.first().restorePath.clear();
    } else if (state == QStringLiteral("large")) {
        draft.verifiedEntries.first().size = qint64(5) * 1024 * 1024 * 1024;
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("manifest.json"));
    QString error;
    QVERIFY2(BackupManifest::write(path, draft, &error), qPrintable(error));
    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    QVERIFY2(BackupManifest::load(path, &entries, &info, &error), qPrintable(error));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().restorePath, QStringLiteral("notes.txt"));
    QCOMPARE(entries.first().sourcePath, draft.verifiedEntries.first().sourcePath);
    QCOMPARE(entries.first().remotePath, draft.verifiedEntries.first().remotePath);
    QCOMPARE(entries.first().size, draft.verifiedEntries.first().size);
    QCOMPARE(entries.first().checksum, draft.verifiedEntries.first().checksum);
    QCOMPARE(info.issues.size(), draft.issues.size());
    if (!draft.issues.isEmpty()) {
        QCOMPARE(info.issues.first().path, draft.issues.first().path);
        QCOMPARE(info.issues.first().phase, draft.issues.first().phase);
        QCOMPARE(info.issues.first().reason, draft.issues.first().reason);
    }
    if (state == QStringLiteral("legacy")) {
        QCOMPARE(info.version, 1);
        QCOMPARE(info.status, QStringLiteral("complete"));
    } else {
        QCOMPARE(info.version, 2);
        QCOMPARE(info.application, QStringLiteral("omacustos"));
        QCOMPARE(info.computerName, draft.metadata.computerName);
        QCOMPARE(info.setId, draft.metadata.setId);
        QCOMPARE(info.setName, draft.metadata.setName);
        QCOMPARE(info.copyId, draft.metadata.copyId);
        QCOMPARE(info.createdAt, draft.metadata.createdAt);
        QCOMPARE(info.expectedItems, draft.expectedItems);
        QCOMPARE(info.failedItems, draft.failedItems);
        QCOMPARE(info.status, state == QStringLiteral("complete") || state == QStringLiteral("large")
            ? QStringLiteral("complete") : QStringLiteral("incomplete"));
    }
}

void BackupManifestTest::rejectsInvalidDraftWithoutReplacingManifest_data()
{
    QTest::addColumn<QString>("fault");
    for (const QString &fault : {QStringLiteral("failed verified entry"), QStringLiteral("duplicate restore"),
             QStringLiteral("duplicate remote"), QStringLiteral("duplicate expected"), QStringLiteral("duplicate failed"),
             QStringLiteral("unexpected entry"), QStringLiteral("unsafe restore"), QStringLiteral("unsafe remote"),
             QStringLiteral("invalid checksum"), QStringLiteral("negative size"), QStringLiteral("invalid provenance"),
             QStringLiteral("invalid timestamp")}) {
        QTest::newRow(qPrintable(fault)) << fault;
    }
}

void BackupManifestTest::rejectsInvalidDraftWithoutReplacingManifest()
{
    QFETCH(QString, fault);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("manifest.json"));
    const BackupManifestDraft original = completeDraft();
    QVERIFY(BackupManifest::write(path, original));
    BackupManifestDraft draft = original;
    if (fault == QStringLiteral("failed verified entry")) draft.failedItems = draft.expectedItems;
    if (fault == QStringLiteral("duplicate restore")) draft.verifiedEntries.append(draft.verifiedEntries.first());
    if (fault == QStringLiteral("duplicate remote")) {
        BackupEntry duplicate = draft.verifiedEntries.first();
        duplicate.restorePath = QStringLiteral("other.txt");
        draft.expectedItems.append(duplicate.restorePath);
        draft.verifiedEntries.append(duplicate);
    }
    if (fault == QStringLiteral("duplicate expected")) draft.expectedItems.append(draft.expectedItems.first());
    if (fault == QStringLiteral("duplicate failed")) draft.failedItems = {QStringLiteral("failed.txt"), QStringLiteral("failed.txt")};
    if (fault == QStringLiteral("unexpected entry")) draft.expectedItems.clear();
    if (fault == QStringLiteral("unsafe restore")) draft.verifiedEntries.first().restorePath = QStringLiteral("../outside");
    if (fault == QStringLiteral("unsafe remote")) draft.verifiedEntries.first().remotePath = QStringLiteral("copy/../outside");
    if (fault == QStringLiteral("invalid checksum")) draft.verifiedEntries.first().checksum = QByteArray("bad");
    if (fault == QStringLiteral("negative size")) draft.verifiedEntries.first().size = -1;
    if (fault == QStringLiteral("invalid provenance")) draft.metadata.setId.clear();
    if (fault == QStringLiteral("invalid timestamp")) draft.metadata.createdAt = {};
    QString error;
    QVERIFY(!BackupManifest::write(path, draft, &error));
    QVERIFY(!error.isEmpty());
    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    QVERIFY(BackupManifest::load(path, &entries, &info));
    QCOMPARE(info.status, QStringLiteral("complete"));
    QCOMPARE(info.expectedItems, original.expectedItems);
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().checksum, original.verifiedEntries.first().checksum);
}

void BackupManifestTest::loadsVersionedEntries()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("manifest.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"version":1,"entries":[{"source":"/home/user/file.txt","remote":"copy/file.txt","size":12,"sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]})");
    file.close();

    QVector<BackupEntry> entries;
    QVERIFY(BackupManifest::load(path, &entries));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().remotePath, QStringLiteral("copy/file.txt"));
    QCOMPARE(entries.first().size, qint64(12));
    QCOMPARE(entries.first().checksum.size(), 32);
}

void BackupManifestTest::restoresOnlyTheSelectedFile()
{
    QTemporaryDir remote;
    QTemporaryDir destination;
    QVERIFY(remote.isValid());
    QVERIFY(destination.isValid());
    QDir().mkpath(remote.filePath(QStringLiteral("copy")));

    QFile selected(remote.filePath(QStringLiteral("copy/selected.txt")));
    QVERIFY(selected.open(QIODevice::WriteOnly));
    selected.write("selected");
    selected.close();
    QFile unrelated(remote.filePath(QStringLiteral("copy/unrelated.txt")));
    QVERIFY(unrelated.open(QIODevice::WriteOnly));
    unrelated.write("unrelated");
    unrelated.close();

    BackupEngine engine;
    LocalProvider provider(remote.path());
    QString error;
    QVERIFY(engine.restoreFile({
        QStringLiteral("/source/selected.txt"),
        QStringLiteral("copy/selected.txt"),
        8,
        QCryptographicHash::hash("selected", QCryptographicHash::Sha256),
    }, destination.path(), provider, &error));
    QVERIFY(QFileInfo::exists(destination.filePath(QStringLiteral("selected.txt"))));
    QVERIFY(!QFileInfo::exists(destination.filePath(QStringLiteral("unrelated.txt"))));
}

void BackupManifestTest::rejectsTamperedRestore()
{
    QTemporaryDir remote;
    QTemporaryDir destination;
    QVERIFY(remote.isValid());
    QVERIFY(destination.isValid());
    QDir().mkpath(remote.filePath(QStringLiteral("copy")));

    QFile selected(remote.filePath(QStringLiteral("copy/selected.txt")));
    QVERIFY(selected.open(QIODevice::WriteOnly));
    selected.write("selected");
    selected.close();

    BackupEngine engine;
    LocalProvider provider(remote.path());
    QString error;
    const BackupEntry entry {
        QStringLiteral("/source/selected.txt"),
        QStringLiteral("copy/selected.txt"),
        8,
        QCryptographicHash::hash("different", QCryptographicHash::Sha256),
    };

    QVERIFY(!engine.restoreFile(entry, destination.path(), provider, &error));
    QCOMPARE(error, QStringLiteral("The restored file failed verification."));
    QVERIFY(!QFileInfo::exists(destination.filePath(QStringLiteral("selected.txt"))));
}

void BackupManifestTest::preservesExistingDestinationWhenRestoreFails()
{
    QTemporaryDir remote;
    QTemporaryDir destination;
    QVERIFY(remote.isValid());
    QVERIFY(destination.isValid());
    QDir().mkpath(remote.filePath(QStringLiteral("copy")));

    QFile selected(remote.filePath(QStringLiteral("copy/selected.txt")));
    QVERIFY(selected.open(QIODevice::WriteOnly));
    selected.write("selected");
    selected.close();
    QFile existing(destination.filePath(QStringLiteral("selected.txt")));
    QVERIFY(existing.open(QIODevice::WriteOnly));
    existing.write("keep this");
    existing.close();

    BackupEngine engine;
    LocalProvider provider(remote.path());
    QString error;
    QVERIFY(!engine.restoreFile({
        QStringLiteral("/source/selected.txt"),
        QStringLiteral("copy/selected.txt"),
        8,
        QCryptographicHash::hash("different", QCryptographicHash::Sha256),
    }, destination.path(), provider, &error));

    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), QByteArray("keep this"));
}

void BackupManifestTest::rejectsRestoreThroughDestinationSymlink()
{
    QTemporaryDir remote;
    QTemporaryDir destination;
    QTemporaryDir outside;
    QVERIFY(remote.isValid());
    QVERIFY(destination.isValid());
    QVERIFY(outside.isValid());
    QDir().mkpath(remote.filePath(QStringLiteral("copy")));

    QFile selected(remote.filePath(QStringLiteral("copy/selected.txt")));
    QVERIFY(selected.open(QIODevice::WriteOnly));
    selected.write("selected");
    selected.close();
    QVERIFY(QFile::link(outside.path(), destination.filePath(QStringLiteral("linked"))));

    BackupEngine engine;
    LocalProvider provider(remote.path());
    QString error;
    QVERIFY(!engine.restoreFile({
        QStringLiteral("linked/selected.txt"),
        QStringLiteral("copy/selected.txt"),
        8,
        QCryptographicHash::hash("selected", QCryptographicHash::Sha256),
    }, destination.path(), provider, &error));
    QCOMPARE(error, QStringLiteral("The restore destination is outside the selected folder."));
}

void BackupManifestTest::rejectsTraversalPaths()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("manifest.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"version":1,"entries":[{"source":"/home/user/file.txt","remote":"../outside","size":12,"sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]})");
    file.close();

    QVector<BackupEntry> entries;
    QString error;
    QVERIFY(!BackupManifest::load(path, &entries, &error));
    QCOMPARE(error, QStringLiteral("The backup manifest contains an unsafe path."));
}

void BackupManifestTest::acceptsDotsInsideFileNames()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("manifest.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"version":1,"entries":[{"source":"/home/user/file.txt","remote":"copy/release..txt","size":12,"sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]})");
    file.close();

    QVector<BackupEntry> entries;
    QVERIFY(BackupManifest::load(path, &entries));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().remotePath, QStringLiteral("copy/release..txt"));
}

void BackupManifestTest::acceptsAbsoluteRemotePaths()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("manifest.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"version":1,"entries":[{"source":"/home/user/file.txt","remote":"/my-files/backups/file.txt","size":12,"sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]})");
    file.close();

    QVector<BackupEntry> entries;
    QVERIFY(BackupManifest::load(path, &entries));
    QCOMPARE(entries.first().remotePath, QStringLiteral("/my-files/backups/file.txt"));
}

void BackupManifestTest::rejectsCompleteCopyWithMissingExpectedEntry()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("manifest.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"version":2,"application":"omacustos","computer":"computer","set_id":"set","set_name":"Set","copy_id":"copy","created_at":"2026-09-28T12:00:00.000Z","status":"complete","expected":["file.txt"],"failed":[],"entries":[]})");
    file.close();

    QVector<BackupEntry> entries;
    QString error;
    QVERIFY(!BackupManifest::load(path, &entries, &error));
    QCOMPARE(error, QStringLiteral("The backup manifest is malformed or unsupported."));
}

void BackupManifestTest::rejectsMalformedEntries()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("manifest.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"version":1,"entries":[{"source":"/home/user/file.txt","remote":"copy/file.txt","size":-1}]})");
    file.close();

    QVector<BackupEntry> entries;
    QString error;
    QVERIFY(!BackupManifest::load(path, &entries, &error));
    QCOMPARE(error, QStringLiteral("The backup manifest contains an unsafe path."));
}

void BackupManifestTest::rejectsFailedEntryPresentedAsVerified()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile file(directory.filePath("manifest.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"version":2,"application":"omacustos","computer":"computer","set_id":"set","copy_id":"copy","created_at":"2026-09-28T12:00:00.000Z","status":"incomplete","expected":["file.txt"],"failed":["file.txt"],"entries":[{"source":"/safe/file.txt","remote":"copy/file.txt","restore":"file.txt","size":12,"sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]})");
    file.close();
    QVector<BackupEntry> entries;
    QVERIFY(!BackupManifest::load(file.fileName(), &entries));
    QVERIFY(entries.isEmpty());
}

void BackupManifestTest::rejectsNullOutput()
{
    QString error;
    QVERIFY(!BackupManifest::load(QStringLiteral("/missing/manifest.json"), nullptr, &error));
    QCOMPARE(error, QStringLiteral("A destination for manifest entries is required."));
}

void BackupManifestTest::rejectsMalformedVersionTwoManifests_data()
{
    QTest::addColumn<QByteArray>("manifest");
    const QJsonObject valid = QJsonDocument::fromJson(R"({"version":2,"application":"omacustos","computer":"computer","set_id":"set","copy_id":"copy","created_at":"2026-09-28T12:00:00.000Z","status":"incomplete","expected":["notes.txt","failed.txt"],"failed":["failed.txt"],"entries":[{"source":"/source/notes.txt","remote":"copy/notes.txt","restore":"notes.txt","size":5,"sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]})").object();
    for (const QString &field : {QString("expected"), QString("failed"), QString("entries")}) {
        QJsonObject root = valid;
        QJsonArray items = root.value(field).toArray();
        items.append(items.first());
        root.insert(field, items);
        QTest::newRow(qPrintable("duplicate " + field)) << QJsonDocument(root).toJson();
    }
    for (const QString &field : {QString("expected"), QString("failed")}) {
        QJsonObject root = valid;
        root.insert(field, QJsonArray {42});
        QTest::newRow(qPrintable("non-string " + field)) << QJsonDocument(root).toJson();
    }
    QJsonObject root = valid;
    root.insert("status", "complete");
    QTest::newRow("complete copy with failed upload") << QJsonDocument(root).toJson();
    root = valid;
    root.insert("created_at", "invalid");
    QTest::newRow("invalid timestamp") << QJsonDocument(root).toJson();
    root = valid;
    root.insert("entries", QJsonArray {42});
    QTest::newRow("non-object entry") << QJsonDocument(root).toJson();
    root = valid;
    root.insert("issues", QJsonArray {42});
    QTest::newRow("malformed issue") << QJsonDocument(root).toJson();
    root = valid;
    root.insert("status", "complete");
    root.insert("expected", QJsonArray {"notes.txt"});
    root.insert("failed", QJsonArray {});
    root.insert("issues", QJsonArray {QJsonObject {{"path", "/source"}, {"phase", "uploading"}, {"reason", "transfer failed"}}});
    QTest::newRow("complete copy with transfer issue") << QJsonDocument(root).toJson();
    root = valid;
    QJsonArray entries = root.value("entries").toArray();
    QJsonObject entry = entries.first().toObject();
    entry.insert("restore", "failed.txt");
    entry.insert("sha256", "bad");
    entries.append(entry);
    root.insert("entries", entries);
    QTest::newRow("invalid entry after valid entry") << QJsonDocument(root).toJson();
    for (const QJsonValue &size : {QJsonValue(1.5), QJsonValue(9223372036854775808.0), QJsonValue("5")}) {
        root = valid;
        entry = root.value("entries").toArray().first().toObject();
        entry.insert("size", size);
        root.insert("entries", QJsonArray {entry});
        QTest::newRow(qPrintable("invalid size " + size.toVariant().toString())) << QJsonDocument(root).toJson();
    }
}

void BackupManifestTest::rejectsMalformedVersionTwoManifests()
{
    QFETCH(QByteArray, manifest);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile file(directory.filePath("manifest.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(manifest), qint64(manifest.size()));
    file.close();

    QVector<BackupEntry> entries;
    BackupManifestInfo info;
    QString error;
    QVERIFY(!BackupManifest::load(file.fileName(), &entries, &info, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(entries.isEmpty());
    QCOMPARE(info.version, 0);
    QVERIFY(info.status.isEmpty());
}

QTEST_MAIN(BackupManifestTest)
#include "backupmanifest_test.moc"

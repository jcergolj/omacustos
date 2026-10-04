#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "../src/backupecleanup.h"
#include "../src/backuprunstore.h"

namespace {

bool write(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size();
}

QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

const QByteArray runState = R"({"runs":[{
    "set_id":"documents","status":"running","reason":"manual","attempts":2,
    "remote_copy_path":"/backups/copy","last_error":"offline",
    "next_attempt":"2026-10-04T12:00:00.000Z",
    "progress":{"total_files":10,"processed_files":2,"total_bytes":1000,"processed_bytes":200,
        "verified_files":1,"verified_bytes":100,"failed_items":1,"current_file":"notes.txt",
        "current_file_bytes":100,"phase":"uploading","elapsed_ms":20000,
        "updated_at":"2026-10-04T12:00:00.000Z"},
    "result":{"reported":true,"manifest_verified":true,"verified_files":1,"verified_bytes":100,
        "issues":[{"path":"bad.txt","phase":"reading","reason":"Permission denied"}]}
}]})";

const QByteArray cleanupState = R"({"sets":{"documents":{
    "decision":"confirmed","targets":["/backups/old","/backups/older"],
    "trashed":["/backups/old"],"completed":["/backups/older"],"last_error":"offline"
}}})";

}

class LocalStateStoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void failuresRetainTheEntireSnapshot_data()
    {
        QTest::addColumn<bool>("cleanup");
        QTest::addColumn<bool>("suppliedBytes");
        QTest::addColumn<QByteArray>("malformed");
        QTest::addColumn<bool>("unreadable");
        for (bool cleanup : {false, true}) {
            const QString prefix = cleanup ? "cleanup: " : "runs: ";
            for (bool suppliedBytes : {false, true}) {
                const QString source = suppliedBytes ? "bytes: " : "file: ";
                const auto row = [&](const QString &name, const QByteArray &malformed) {
                    QTest::newRow(qPrintable(prefix + source + name)) << cleanup << suppliedBytes << malformed << false;
                };
                row("invalid JSON", "{");
                row("empty present file", "");
                row("non-object document", "[]");
                row("wrong collection type", cleanup ? R"({"sets":[]})" : R"({"runs":{}})");
                row("invalid record after valid record", cleanup
                    ? R"({"sets":{"a-valid":{"decision":"pending"},"z-invalid":false}})"
                    : R"({"runs":[{"set_id":"new"},{}]})");
            }
            QTest::newRow(qPrintable(prefix + "unreadable file")) << cleanup << false << QByteArray() << true;
        }
    }

    void failuresRetainTheEntireSnapshot()
    {
        QFETCH(bool, cleanup);
        QFETCH(bool, suppliedBytes);
        QFETCH(QByteArray, malformed);
        QFETCH(bool, unreadable);
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QString path = home.filePath("state.json");
        const auto exercise = [&](auto &store, const QByteArray &initial) {
            QVERIFY(write(path, initial));
            QVERIFY(store.load());
            QVERIFY(store.save());
            const QByteArray lastGood = read(path);
            QByteArray captured;
            QVERIFY(store.load(nullptr, &captured));
            QCOMPARE(captured, QByteArray(1, '\1') + lastGood);
            if (unreadable) {
                QVERIFY(QFile::remove(path));
                QVERIFY(QDir().mkdir(path));
            } else {
                QVERIFY(write(path, malformed));
            }
            QString error;
            if (suppliedBytes) {
                QVERIFY(!store.loadFromBytes(malformed, &error));
            } else {
                QVERIFY(!store.load(&error, &captured));
                QCOMPARE(captured, QByteArray(1, '\1') + lastGood);
            }
            QVERIFY(!error.isEmpty());
            if (unreadable) QVERIFY(QDir().rmdir(path));
            QVERIFY(store.save());
            QCOMPARE(read(path), lastGood); // Includes every persisted field, with no partial replacement.
        };
        if (cleanup) {
            CleanupStore store(path);
            exercise(store, cleanupState);
        } else {
            BackupRunStore store(path);
            exercise(store, runState);
        }
    }

    void capturedBytesReplaceStateWithoutReopeningTheFile_data()
    {
        QTest::addColumn<bool>("cleanup");
        QTest::newRow("cleanup") << true;
        QTest::newRow("runs") << false;
    }

    void capturedBytesReplaceStateWithoutReopeningTheFile()
    {
        QFETCH(bool, cleanup);
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QString path = home.filePath("state.json");
        const auto exercise = [&](auto &store, const QByteArray &initial) {
            QVERIFY(write(path, initial));
            QVERIFY(store.load());
            QVERIFY(store.save());
            const QByteArray captured = read(path);
            QVERIFY(store.loadFromBytes("{}"));
            QVERIFY(write(path, "{"));
            QVERIFY(store.loadFromBytes(captured));
            QVERIFY(store.save());
            QCOMPARE(read(path), captured);
        };
        if (cleanup) {
            CleanupStore store(path);
            exercise(store, cleanupState);
        } else {
            BackupRunStore store(path);
            exercise(store, runState);
        }
    }

    void missingFilesClearStateAndDifferFromEmptyFiles_data()
    {
        capturedBytesReplaceStateWithoutReopeningTheFile_data();
    }

    void missingFilesClearStateAndDifferFromEmptyFiles()
    {
        QFETCH(bool, cleanup);
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QString path = home.filePath("state.json");
        const auto exercise = [&](auto &store, const QByteArray &initial) {
            QVERIFY(store.loadFromBytes(initial));
            QByteArray captured("previous");
            QVERIFY(store.load(nullptr, &captured));
            QCOMPARE(captured, QByteArray(1, '\0'));
            QVERIFY(store.save());
            const QByteArray emptyState = read(path);
            QVERIFY(store.load(nullptr, &captured));
            QCOMPARE(captured, QByteArray(1, '\1') + emptyState);
            QVERIFY(write(path, ""));
            QString error;
            QVERIFY(!store.load(&error, &captured));
            QVERIFY(!error.isEmpty());
            QCOMPARE(captured, QByteArray(1, '\1') + emptyState);
        };
        if (cleanup) {
            CleanupStore store(path);
            exercise(store, cleanupState);
            QVERIFY(store.states().isEmpty());
        } else {
            BackupRunStore store(path);
            exercise(store, runState);
            QVERIFY(store.records().isEmpty());
        }
    }
};

QTEST_GUILESS_MAIN(LocalStateStoreTest)
#include "localstate_store_test.moc"

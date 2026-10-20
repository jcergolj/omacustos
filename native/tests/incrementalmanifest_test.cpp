#include "incrementalmanifest.h"
#include "backupmanifest.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

namespace {
const QString setId = "10000000-0000-4000-8000-000000000001";
const QString snapshotId = "30000000-0000-4000-8000-000000000001";

QByteArray fixture(const QString &name)
{
    QFile file(QStringLiteral(OMACUSTOS_SOURCE_DIR) + "/tests/fixtures/incremental/" + name + ".json");
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

QJsonObject baseline()
{
    return QJsonDocument::fromJson(fixture("baseline")).object();
}

bool valid(const QJsonObject &root)
{
    return IncrementalManifest::validate(QJsonDocument(root).toJson(), setId, snapshotId);
}
}

class IncrementalManifestTest : public QObject
{
    Q_OBJECT
private slots:
    void independentSharedSubset()
    {
        QString error = "stale";
        QVERIFY2(IncrementalManifest::validate(fixture("baseline"), setId, snapshotId, &error), qPrintable(error));
        QVERIFY(error.isEmpty());
        QVERIFY2(IncrementalManifest::validate(fixture("subset"), setId,
            "30000000-0000-4000-8000-000000000002", &error), qPrintable(error));
        // Both examples embed the same complete inventory, not an ancestor pointer.
        const auto older = baseline();
        const auto newer = QJsonDocument::fromJson(fixture("subset")).object();
        QCOMPARE(older.value("archives"), newer.value("archives"));
        QCOMPARE(newer.value("entries").toArray().size(), 1);
        QVERIFY(!IncrementalManifest::validate(fixture("subset"), setId, snapshotId));
        QVERIFY(!IncrementalManifest::validate(fixture("baseline"),
            "10000000-0000-4000-8000-000000000002", snapshotId));
    }

    void incompatibleAndMalformed()
    {
        for (const auto &value : {QJsonValue(3), QJsonValue(5), QJsonValue(4.1), QJsonValue("4"), QJsonValue()}) {
            auto root = baseline();
            root["version"] = value;
            QVERIFY(!valid(root));
        }
        for (const QString &key : baseline().keys()) {
            auto root = baseline();
            root.remove(key);
            QVERIFY2(!valid(root), qPrintable(key));
        }
        auto root = baseline();
        root["ancestor"] = snapshotId;
        QVERIFY(!valid(root));
        root = baseline();
        root["status"] = "incomplete";
        QVERIFY(!valid(root));
        root = baseline();
        root["ownership_epoch"] = 1.5;
        QVERIFY(!valid(root));
        QString error;
        QVERIFY(!IncrementalManifest::validate("{", setId, snapshotId, &error));
        QVERIFY(!error.isEmpty());
    }

    void unsafePaths_data()
    {
        QTest::addColumn<QString>("name");
        for (const QString &name : {QString("../escape"), QString("/absolute"), QString("a/../b"),
                QString("a//b"), QString("a/./b"), QString("a/"), QString("a\\b"), QString("a\nb"),
                QString("."), QString(), QString("a") + QChar::Null})
            QTest::newRow(qPrintable(name.toUtf8().toHex())) << name;
    }

    void unsafePaths()
    {
        QFETCH(QString, name);
        auto root = baseline();
        auto entries = root["entries"].toArray();
        auto entry = entries[0].toObject();
        entry["restore"] = name;
        entries[0] = entry;
        root["entries"] = entries;
        QVERIFY(!valid(root));
        root = baseline();
        auto archives = root["archives"].toArray();
        auto archive = archives[0].toObject();
        archive["remote"] = name;
        archives[0] = archive;
        root["archives"] = archives;
        QVERIFY(!valid(root));
        auto members = archive["members"].toArray();
        auto member = members[0].toObject();
        member["path"] = name;
        members[0] = member;
        archive = baseline()["archives"].toArray()[0].toObject();
        archive["members"] = members;
        archives[0] = archive;
        root["archives"] = archives;
        QVERIFY(!valid(root));
    }

    void malformedInventories()
    {
        const auto original = baseline()["archives"].toArray()[0].toObject();
        QList<QJsonObject> bad;
        for (const QString &key : original.keys()) {
            auto archive = original;
            archive.remove(key);
            bad.append(archive);
        }
        auto archive = original;
        archive["size"] = 1.5;
        bad.append(archive);
        archive = original;
        archive["size"] = 0;
        bad.append(archive);
        archive = original;
        archive["remote"] = "../other-set/objects/" + archive["id"].toString() + ".tar.gz";
        bad.append(archive);
        for (const QString &method : {QString("size-only"), QString()}) {
            archive = original;
            auto receipt = archive["original_verification"].toObject();
            receipt["method"] = method;
            archive["original_verification"] = receipt;
            bad.append(archive);
        }
        archive = original;
        auto receipt = archive["original_verification"].toObject();
        receipt["sha256"] = QString(64, 'd');
        archive["original_verification"] = receipt;
        bad.append(archive);
        for (int scenario = 0; scenario < 5; ++scenario) {
            archive = original;
            auto members = archive["members"].toArray();
            auto member = members[0].toObject();
            if (scenario == 0) members.append(member);
            if (scenario == 1) members = {};
            if (scenario == 2) { member["sha256"] = "not-a-hash"; members[0] = member; }
            if (scenario == 3) { member["size"] = -1; members[0] = member; }
            if (scenario == 4) { member["path"] = "a/child"; members[1] = member; }
            archive["members"] = members;
            bad.append(archive);
        }
        for (const auto &item : bad) {
            auto root = baseline();
            root["archives"] = QJsonArray{item};
            QVERIFY(!valid(root));
        }
        auto root = baseline();
        root["archives"] = QJsonArray{original, original};
        QVERIFY(!valid(root));
        root["entries"] = QJsonArray{};
        root["archives"] = QJsonArray{original};
        QVERIFY(!valid(root)); // No entirely unused archive in a snapshot.
        root["archives"] = QJsonArray{};
        QVERIFY(valid(root)); // A saved empty selection is a valid complete state.
    }

    void malformedLogicalReferences()
    {
        for (const QString &key : {QString("archive"), QString("member"), QString("size"), QString("sha256"),
                QString("mode"), QString("mtime_ns")}) {
            auto root = baseline();
            auto entries = root["entries"].toArray();
            auto entry = entries[0].toObject();
            if (key == "archive") entry[key] = "40000000-0000-4000-8000-000000000002";
            if (key == "member") entry[key] = "missing";
            if (key == "size") entry[key] = 2;
            if (key == "sha256") entry[key] = QString(64, 'd');
            if (key == "mode") entry[key] = 4095;
            if (key == "mtime_ns") entry[key] = "9223372036854775808";
            entries[0] = entry;
            root["entries"] = entries;
            QVERIFY2(!valid(root), qPrintable(key));
        }
        for (const QString &destination : {QString("project/a"), QString("project/a/child")}) {
            auto root = baseline();
            auto entries = root["entries"].toArray();
            auto entry = entries[1].toObject();
            entry["restore"] = destination;
            entries[1] = entry;
            root["entries"] = entries;
            QVERIFY(!valid(root));
        }
    }

    void legacyReaderStaysClosed()
    {
        QVector<BackupEntry> entries;
        QString error;
        QVERIFY(!BackupManifest::load(QStringLiteral(OMACUSTOS_SOURCE_DIR)
            + "/tests/fixtures/incremental/subset.json", &entries, &error));
        QVERIFY(entries.isEmpty());
    }
};

QTEST_GUILESS_MAIN(IncrementalManifestTest)
#include "incrementalmanifest_test.moc"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include "../src/backupconfig.h"

class BackupConfigTest final : public QObject
{
    Q_OBJECT

private slots:
    void loadsLegacyConfigurationAndSavesModernSets_data();
    void loadsLegacyConfigurationAndSavesModernSets();
    void modernSetsOverrideLegacyFields_data();
    void modernSetsOverrideLegacyFields();
    void savesAndLoadsIndependentSets();
    void savesAndLoadsEmptySetList();
    void ignoresAndDropsLegacyExternalDriveRequirements();
    void rejectsMalformedConfiguration();
    void rejectsIncompleteConfiguration();
    void rejectsDuplicateSetIdsAndInvalidSchedules();
    void rejectsNullOutput();
    void rejectsMalformedInclusions_data();
    void rejectsMalformedInclusions();
};

void BackupConfigTest::loadsLegacyConfigurationAndSavesModernSets_data()
{
    QTest::addColumn<bool>("customBinary");
    QTest::newRow("default CLI") << false;
    QTest::newRow("configured CLI") << true;
}

void BackupConfigTest::rejectsMalformedInclusions_data()
{
    QTest::addColumn<QJsonValue>("rules");
    QTest::newRow("string") << QJsonValue(".env");
    QTest::newRow("null") << QJsonValue(QJsonValue::Null);
    QTest::newRow("non-string entry") << QJsonValue(QJsonArray {".env", false});
}

void BackupConfigTest::rejectsMalformedInclusions()
{
    QFETCH(QJsonValue, rules);
    QTemporaryDir directory;
    BackupConfigStore store(directory.filePath("settings.json"));
    QFile file(store.filePath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(QJsonObject {{"sets", QJsonArray {QJsonObject {
        {"id", "env"}, {"name", "Env"}, {"remote_root", "/backups"},
        {"source_directories", QJsonArray {"/safe/projects"}}, {"inclusions", rules},
    }}}}).toJson());
    file.close();
    BackupConfig config;
    QString error;
    QVERIFY(!store.load(&config, &error));
    QVERIFY(!error.isEmpty());
}

void BackupConfigTest::loadsLegacyConfigurationAndSavesModernSets()
{
    QFETCH(bool, customBinary);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    BackupConfigStore store(directory.filePath(QStringLiteral("settings.json")));
    QJsonObject legacy {
        {"source_directory", "/home/user/Documents"},
        {"remote_root", "/my-files/backups/computer/copy"},
    };
    if (customBinary) legacy.insert("proton_binary", "/custom/proton-drive");
    QFile file(store.filePath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(legacy).toJson());
    file.close();

    BackupConfig actual;
    QVERIFY(store.load(&actual));
    const QString binary = customBinary ? "/custom/proton-drive" : "proton-drive";
    QCOMPARE(actual.protonBinary, binary);
    QCOMPARE(actual.sets.size(), 1);
    QCOMPARE(actual.sets.first().id, QStringLiteral("default"));
    QCOMPARE(actual.sets.first().name, QStringLiteral("Default backup"));
    QCOMPARE(actual.sets.first().sourceDirectories, QStringList {QStringLiteral("/home/user/Documents")});
    QCOMPARE(actual.sets.first().remoteRoot, QStringLiteral("/my-files/backups/computer/copy"));
    QCOMPARE(actual.sets.first().schedule.frequency, QStringLiteral("disabled"));
    QCOMPARE(actual.sets.first().retention, 3);
    QVERIFY(!actual.sets.first().onlyOnAcPower);
    QVERIFY(actual.sets.first().exclusions.isEmpty());
    QVERIFY(actual.sets.first().inclusions.isEmpty());

    QVERIFY(store.save(actual));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject modern = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QCOMPARE(modern.value("sets").toArray().size(), 1);
    QVERIFY(!modern.contains("source_directory"));
    QVERIFY(!modern.contains("remote_root"));
    BackupConfig reloaded;
    QVERIFY(store.load(&reloaded));
    QCOMPARE(reloaded.protonBinary, binary);
    QCOMPARE(reloaded.sets.first().id, actual.sets.first().id);
    QCOMPARE(reloaded.sets.first().name, actual.sets.first().name);
    QCOMPARE(reloaded.sets.first().sourceDirectories, actual.sets.first().sourceDirectories);
    QCOMPARE(reloaded.sets.first().remoteRoot, actual.sets.first().remoteRoot);
}

void BackupConfigTest::modernSetsOverrideLegacyFields_data()
{
    QTest::addColumn<bool>("empty");
    QTest::newRow("modern backup") << false;
    QTest::newRow("explicitly empty list") << true;
}

void BackupConfigTest::modernSetsOverrideLegacyFields()
{
    QFETCH(bool, empty);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    BackupConfigStore store(directory.filePath("settings.json"));
    QJsonArray sets;
    if (!empty) {
        sets.append(QJsonObject {
            {"id", "documents"}, {"name", "Documents"}, {"remote_root", "/modern/backups"},
            {"source_directories", QJsonArray {"/modern/documents"}},
        });
    }
    QFile file(store.filePath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(QJsonObject {
        {"source_directory", "/legacy/documents"}, {"remote_root", "/legacy/backups"}, {"sets", sets},
    }).toJson());
    file.close();
    BackupConfig config;
    config.sets = {{"stale", "Stale", "/stale/backups", {"/stale/documents"}, {}}};
    QVERIFY(store.load(&config));
    QCOMPARE(config.sets.size(), empty ? 0 : 1);
    if (!empty) {
        QCOMPARE(config.sets.first().id, QStringLiteral("documents"));
        QCOMPARE(config.sets.first().sourceDirectories, QStringList {"/modern/documents"});
        QCOMPARE(config.sets.first().remoteRoot, QStringLiteral("/modern/backups"));
    }
    QVERIFY(store.save(config));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject modern = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QVERIFY(!modern.contains("source_directory"));
    QVERIFY(!modern.contains("remote_root"));
    QVERIFY(store.load(&config));
    QCOMPARE(config.sets.size(), empty ? 0 : 1);
}

void BackupConfigTest::savesAndLoadsIndependentSets()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    BackupConfigStore store(directory.filePath(QStringLiteral("config/settings.json")));
    BackupConfig expected;
    expected.protonBinary = QStringLiteral("/usr/bin/proton-drive");
    expected.sets = {
        {
            QStringLiteral("documents"),
            QStringLiteral("Documents"),
            QStringLiteral("backups/documents"),
            {QStringLiteral("/home/user/Documents"), QStringLiteral("/home/user/Notes")},
            {QStringLiteral("/home/user/Documents/cache")},
        },
        {
            QStringLiteral("configs"),
            QStringLiteral("Configs"),
            QStringLiteral("backups/configs"),
            {QStringLiteral("/home/user/.config")},
            {},
        },
    };
    expected.sets[0].schedule = {QStringLiteral("monthly"), 8, 45, 2, 31};
    expected.sets[0].retention = 5;
    expected.sets[0].onlyOnAcPower = true;
    expected.sets[0].inclusions = {".env", "config", "project/.env*"};

    QVERIFY(store.save(expected));
    BackupConfig actual;
    QVERIFY(store.load(&actual));
    QCOMPARE(actual.protonBinary, expected.protonBinary);
    QCOMPARE(actual.sets.size(), 2);
    QCOMPARE(actual.sets.at(0).id, expected.sets.at(0).id);
    QCOMPARE(actual.sets.at(0).name, QStringLiteral("Documents"));
    QCOMPARE(actual.sets.at(0).sourceDirectories, expected.sets.at(0).sourceDirectories);
    QCOMPARE(actual.sets.at(0).exclusions, expected.sets.at(0).exclusions);
    QCOMPARE(actual.sets.at(0).inclusions, expected.sets.at(0).inclusions);
    QCOMPARE(actual.sets.at(0).schedule.frequency, QStringLiteral("monthly"));
    QCOMPARE(actual.sets.at(0).schedule.hour, 8);
    QCOMPARE(actual.sets.at(0).schedule.minute, 45);
    QCOMPARE(actual.sets.at(0).schedule.weekday, 2);
    QCOMPARE(actual.sets.at(0).schedule.dayOfMonth, 31);
    QCOMPARE(actual.sets.at(0).retention, 5);
    QCOMPARE(actual.sets.at(0).onlyOnAcPower, true);
    QCOMPARE(actual.sets.at(1).remoteRoot, QStringLiteral("backups/configs"));
}

void BackupConfigTest::savesAndLoadsEmptySetList()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    BackupConfigStore store(directory.filePath(QStringLiteral("config/settings.json")));
    BackupConfig expected;
    expected.protonBinary = QStringLiteral("/usr/bin/proton-drive");

    QVERIFY(store.save(expected));
    BackupConfig actual;
    QVERIFY(store.load(&actual));
    QCOMPARE(actual.protonBinary, expected.protonBinary);
    QVERIFY(actual.sets.isEmpty());
}

void BackupConfigTest::ignoresAndDropsLegacyExternalDriveRequirements()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"proton_binary":"proton-drive","sets":[{
        "id":"documents","name":"Documents","remote_root":"/my-files/backups",
        "source_directories":["/home/user/Documents"],"exclusions":["/home/user/Documents/cache"],
        "schedule":{"frequency":"daily","hour":9,"minute":30},"only_on_ac_power":true,
        "required_volumes":[{"mount_path":"/run/media/missing-drive","device_id":"646576696365"}]
    }]})");
    file.close();

    BackupConfigStore store(path);
    BackupConfig config;
    QVERIFY(store.load(&config));
    QCOMPARE(config.sets.size(), 1);
    QCOMPARE(config.sets.first().id, QStringLiteral("documents"));
    QCOMPARE(config.sets.first().name, QStringLiteral("Documents"));
    QCOMPARE(config.sets.first().exclusions, QStringList {QStringLiteral("/home/user/Documents/cache")});
    QCOMPARE(config.sets.first().schedule.frequency, QStringLiteral("daily"));
    QCOMPARE(config.sets.first().schedule.hour, 9);
    QCOMPARE(config.sets.first().schedule.minute, 30);
    QVERIFY(config.sets.first().onlyOnAcPower);

    QVERIFY(store.save(config));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject backup = QJsonDocument::fromJson(file.readAll()).object()
        .value(QStringLiteral("sets")).toArray().first().toObject();
    QVERIFY(!backup.contains(QStringLiteral("required_volumes")));
}

void BackupConfigTest::rejectsMalformedConfiguration()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("not-json");
    file.close();

    BackupConfigStore store(path);
    BackupConfig config;
    QString error;
    QVERIFY(!store.load(&config, &error));
    QCOMPARE(error, QStringLiteral("The OmaCustos backup configuration is malformed."));
}

void BackupConfigTest::rejectsIncompleteConfiguration()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    BackupConfigStore store(directory.filePath(QStringLiteral("settings.json")));
    BackupConfig config;
    config.protonBinary.clear();
    QString error;

    QVERIFY(!store.save(config, &error));
    QCOMPARE(error, QStringLiteral("The OmaCustos backup configuration is incomplete."));
}

void BackupConfigTest::rejectsDuplicateSetIdsAndInvalidSchedules()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    BackupConfigStore store(directory.filePath(QStringLiteral("settings.json")));
    BackupConfig config;
    config.protonBinary = QStringLiteral("proton-drive");
    config.sets = {
        {QStringLiteral("same"), QStringLiteral("One"), QStringLiteral("backups/one"), {QStringLiteral("/tmp")}},
        {QStringLiteral("same"), QStringLiteral("Two"), QStringLiteral("backups/two"), {QStringLiteral("/tmp")}},
    };
    QString error;
    QVERIFY(!store.save(config, &error));
    QCOMPARE(error, QStringLiteral("The OmaCustos backup configuration is incomplete."));

    config.sets.removeLast();
    config.sets.first().schedule.frequency = QStringLiteral("hourly");
    QVERIFY(!store.save(config, &error));
    QCOMPARE(error, QStringLiteral("The OmaCustos backup configuration is incomplete."));
}

void BackupConfigTest::rejectsNullOutput()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    BackupConfigStore store(directory.filePath(QStringLiteral("settings.json")));
    QString error;

    QVERIFY(!store.load(nullptr, &error));
    QCOMPARE(error, QStringLiteral("A destination for OmaCustos backup configuration is required."));
}

QTEST_MAIN(BackupConfigTest)
#include "backupconfig_test.moc"

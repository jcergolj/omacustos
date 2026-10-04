#include "../src/backuplauncher.h"
#include "../src/backupconfig.h"
#include "../src/backuprunstore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

namespace {

class EnvironmentOverride
{
public:
    EnvironmentOverride(const char *name, const QByteArray &value)
        : name(name), wasSet(qEnvironmentVariableIsSet(name)), previous(qgetenv(name))
    {
        qputenv(name, value);
    }

    ~EnvironmentOverride()
    {
        if (wasSet) qputenv(name.constData(), previous);
        else qunsetenv(name.constData());
    }

private:
    QByteArray name;
    bool wasSet;
    QByteArray previous;
};

bool write(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

}

class BackupLauncherTest final : public QObject
{
    Q_OBJECT

private slots:
    void queuesConfiguredBackups_data()
    {
        QTest::addColumn<QString>("entryPoint");
        QTest::newRow("legacy source/destination entry point") << "legacy-launcher";
        QTest::newRow("legacy file consumer") << "legacy-file";
        QTest::newRow("modern file consumer") << "modern-file";
        QTest::newRow("selected modern set") << "selected-set";
    }

    void queuesConfiguredBackups()
    {
        QFETCH(QString, entryPoint);
        QTemporaryDir home;
        QVERIFY(home.isValid());
        EnvironmentOverride homeOverride("HOME", home.path().toUtf8());
        EnvironmentOverride pathOverride("PATH", home.path().toUtf8());
        EnvironmentOverride binaryOverride("OMACUSTOS_PROTON_BIN", "/custom/proton-drive");
        const QString systemctl = home.filePath("systemctl");
        const QByteArray script = "#!/bin/sh\n"
            "if [ \"$*\" != '--user --no-block start omacustos.service' ]; then exit 2; fi\n"
            "printf '%s\\n' \"$*\" > \"${0%/*}/calls\"\n";
        QVERIFY(write(systemctl, script));
        QVERIFY(QFile::setPermissions(systemctl, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const QString configPath = home.filePath(".config/omacustos/omacustos-backup.json");
        const QString runPath = home.filePath(".config/omacustos/omacustos-backup-runs.json");
        const QString source = home.filePath("Documents");
        const QString remote = "/my-files/backups";
        const bool legacy = entryPoint.startsWith("legacy");
        const QString id = legacy ? "default" : "documents";
        if (entryPoint == "legacy-file") {
            QVERIFY(QDir().mkpath(QFileInfo(configPath).absolutePath()));
            QVERIFY(write(configPath, QJsonDocument(QJsonObject {
                {"source_directory", source}, {"remote_root", remote}, {"proton_binary", "/custom/proton-drive"},
            }).toJson()));
        } else if (!legacy) {
            BackupConfig config;
            config.protonBinary = "/custom/proton-drive";
            config.sets = {
                {id, "Documents", remote, {source}, {}},
                {"photos", "Photos", remote, {home.filePath("Photos")}, {}},
            };
            QVERIFY(BackupConfigStore(configPath).save(config));
        }

        BackupLauncher launcher;
        QSignalSpy started(&launcher, &BackupLauncher::started);
        QSignalSpy failed(&launcher, &BackupLauncher::failed);
        if (entryPoint == "legacy-launcher") launcher.startBackup(source, remote);
        else if (entryPoint == "selected-set") launcher.startBackup(id);
        else launcher.startBackup();
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(started.count(), 1);

        BackupConfig saved;
        QVERIFY(BackupConfigStore(configPath).load(&saved));
        QCOMPARE(saved.protonBinary, QStringLiteral("/custom/proton-drive"));
        QCOMPARE(saved.sets.first().id, id);
        QCOMPARE(saved.sets.first().name, legacy ? QString("Default backup") : QString("Documents"));
        QCOMPARE(saved.sets.first().sourceDirectories, QStringList {source});
        QCOMPARE(saved.sets.first().remoteRoot, remote);
        if (entryPoint == "legacy-launcher") {
            QFile file(configPath);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();
            QCOMPARE(document.value("sets").toArray().size(), 1);
            QVERIFY(!document.contains("source_directory"));
            QVERIFY(!document.contains("remote_root"));
        }
        BackupRunStore runs(runPath);
        QVERIFY(runs.load());
        QVERIFY(runs.find(id) != nullptr);
        QCOMPARE(runs.find(id)->status, QStringLiteral("pending"));
        QCOMPARE(runs.find(id)->reason, QStringLiteral("manual"));
        if (entryPoint == "modern-file") {
            QVERIFY(runs.find("photos") != nullptr);
            QCOMPARE(runs.find("photos")->status, QStringLiteral("pending"));
        } else {
            QVERIFY(runs.find("photos") == nullptr);
        }
        QFile calls(home.filePath("calls"));
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(calls.readAll(), QByteArray("--user --no-block start omacustos.service\n"));
    }
};

QTEST_GUILESS_MAIN(BackupLauncherTest)
#include "backuplauncher_test.moc"

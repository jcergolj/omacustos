#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include "backupschedule.h"

struct BackupSet {
    QString id;
    QString name;
    QString remoteRoot;
    QStringList sourceDirectories;
    QStringList exclusions;
    BackupSchedule schedule;
    int retention = 3;
    bool onlyOnAcPower = false;

    QString remoteFolder(const QString &computerName) const;
};

struct BackupConfig {
    QString protonBinary = QStringLiteral("proton-drive");
    QVector<BackupSet> sets;
};

class BackupConfigStore
{
public:
    explicit BackupConfigStore(QString path);

    bool load(BackupConfig *config, QString *error = nullptr) const;
    bool save(const BackupConfig &config, QString *error = nullptr) const;
    bool exportSets(const BackupConfig &config, QString *error = nullptr) const;
    bool importSets(BackupConfig *config, QString *error = nullptr) const;
    QString filePath() const;

private:
    bool loadFile(BackupConfig *config, bool setsOnly, QString *error) const;
    bool saveFile(const BackupConfig &config, bool setsOnly, QString *error) const;
    QString path;
};

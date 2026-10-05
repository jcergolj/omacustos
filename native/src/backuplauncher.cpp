#include "backuplauncher.h"

#include "backuprunstore.h"

#include <QDir>

#include <algorithm>

BackupLauncher::BackupLauncher(QObject *parent)
    : QObject(parent)
    , runner(QStringLiteral("systemctl"))
    , systemd(runner)
{
}

void BackupLauncher::startBackup()
{
    const QString configPath = QDir::home().filePath(QStringLiteral(".config/omacustos/omacustos-backup.json"));
    BackupConfig config;
    QString error;
    if (!BackupConfigStore(configPath).load(&config, &error)) {
        emit failed(error);

        return;
    }

    const QString runPath = QDir::home().filePath(QStringLiteral(".config/omacustos/omacustos-backup-runs.json"));
    BackupRunStore runs(runPath);
    QStringList setIds;
    for (const BackupSet &set : config.sets) {
        setIds.append(set.id);
    }
    if (!runs.queueManual(setIds, false, &error)) {
        emit failed(error);

        return;
    }

    startService();
}

void BackupLauncher::startBackup(const QString &setId)
{
    const QString configPath = QDir::home().filePath(QStringLiteral(".config/omacustos/omacustos-backup.json"));
    BackupConfig config;
    QString error;
    if (!BackupConfigStore(configPath).load(&config, &error)) {
        emit failed(error);
        return;
    }
    const auto set = std::find_if(config.sets.cbegin(), config.sets.cend(), [&setId](const BackupSet &candidate) {
        return candidate.id == setId;
    });
    if (set == config.sets.cend()) {
        emit failed(QStringLiteral("The selected backup no longer exists."));
        return;
    }
    const QString runPath = QDir::home().filePath(QStringLiteral(".config/omacustos/omacustos-backup-runs.json"));
    BackupRunStore runs(runPath);
    if (!runs.queueManual({setId}, true, &error)) {
        emit failed(error);
        return;
    }
    startService();
}

void BackupLauncher::startBackup(const QString &sourceDirectory, const QString &remoteRoot)
{
    BackupConfig config;
    config.protonBinary = qEnvironmentVariable("OMACUSTOS_PROTON_BIN", QStringLiteral("proton-drive"));
    config.sets = {{QStringLiteral("default"), QStringLiteral("Default backup"), remoteRoot, {sourceDirectory}, {}}};
    const QString configPath = QDir::home().filePath(QStringLiteral(".config/omacustos/omacustos-backup.json"));
    QString error;
    if (!BackupConfigStore(configPath).save(config, &error)) {
        emit failed(error);

        return;
    }

    const QString runPath = QDir::home().filePath(QStringLiteral(".config/omacustos/omacustos-backup-runs.json"));
    BackupRunStore runs(runPath);
    if (!runs.queueManual({QStringLiteral("default")}, false, &error)) {
        emit failed(error.isEmpty() ? QStringLiteral("Unable to queue the backup.") : error);

        return;
    }

    startService();
}

void BackupLauncher::startService()
{
    QString error;
    if (!systemd.startUserService(QStringLiteral("omacustos.service"), &error)) {
        emit failed(error);

        return;
    }

    emit started();
}

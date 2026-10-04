#include "backuplauncher.h"

#include "backuprunstore.h"

#include <QDir>
#include <QLockFile>

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
    QLockFile runStateLock(runPath + QStringLiteral(".lock"));
    if (!runStateLock.tryLock(0)) {
        emit failed(QStringLiteral("A backup queue update is already in progress."));
        return;
    }
    BackupRunStore runs(runPath);
    if (!runs.load(&error)) {
        emit failed(error);

        return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    for (const BackupSet &set : config.sets) {
        runs.enqueue(set.id, QStringLiteral("manual"), now);
    }
    if (!runs.save(&error)) {
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
    QLockFile runStateLock(runPath + QStringLiteral(".lock"));
    if (!runStateLock.tryLock(0)) {
        emit failed(QStringLiteral("A backup queue update is already in progress."));
        return;
    }
    BackupRunStore runs(runPath);
    if (!runs.load(&error) || !runs.enqueue(setId, QStringLiteral("manual"), QDateTime::currentDateTime())) {
        emit failed(error.isEmpty() ? QStringLiteral("The selected backup is already queued.") : error);
        return;
    }
    if (!runs.save(&error)) {
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
    QLockFile runStateLock(runPath + QStringLiteral(".lock"));
    if (!runStateLock.tryLock(0)) {
        emit failed(QStringLiteral("A backup queue update is already in progress."));
        return;
    }
    BackupRunStore runs(runPath);
    if (!runs.load(&error)) {
        emit failed(error.isEmpty() ? QStringLiteral("Unable to queue the backup.") : error);

        return;
    }
    runs.enqueue(QStringLiteral("default"), QStringLiteral("manual"), QDateTime::currentDateTime());
    if (!runs.save(&error)) {
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

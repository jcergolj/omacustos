#pragma once

#include "backupconfig.h"
#include "processrunner.h"
#include "qprocessrunner.h"
#include "systemdlauncher.h"

#include <QObject>

class BackupLauncher final : public QObject
{
    Q_OBJECT

public:
    Q_INVOKABLE void pauseBackup(const QString &setId);
    Q_INVOKABLE void resumeBackup(const QString &setId);
    Q_INVOKABLE void cancelBackup(const QString &setId);
    explicit BackupLauncher(QObject *parent = nullptr);

public slots:
    void startBackup();
    void startBackup(const QString &setId);
    void startBackup(const QString &sourceDirectory, const QString &remoteRoot);

signals:
    void started();
    void failed(const QString &error);

private:
    void requestControl(const QString &setId, const QString &action);
    void startService();

    QProcessRunner runner;
    SystemdLauncher systemd;
};

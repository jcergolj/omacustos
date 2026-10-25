#pragma once
#include "backupprovider.h"
#include <QObject>
#include <QFutureWatcher>
#include <QTemporaryDir>
#include <QVariantMap>
#include <atomic>

class CloudSettingsController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QVariantList versions READ versions NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    CloudSettingsController(BackupProvider &provider, QString configPath, QObject *parent = nullptr);
    ~CloudSettingsController() override;
    bool busy() const { return working; }
    QVariantList versions() const { return rows; }
    QString error() const { return lastError; }
    bool stopRequested() const { return stopped.load(); }
    Q_INVOKABLE void browse();
    Q_INVOKABLE void exportSettings();
    Q_INVOKABLE void previewVersion(const QString &path);
signals:
    void changed();
    void downloaded(const QString &filePath);
    void exported(const QString &remotePath);
private:
    void start(const QString &action, const QString &selection = {});
    BackupProvider &provider;
    QString configPath;
    QTemporaryDir workspace;
    QFutureWatcher<QVariantMap> watcher;
    QVariantList rows;
    QString lastError;
    bool working = false;
    std::atomic_bool stopped{false};
};

#pragma once

#include "backupprovider.h"
#include "processrunner.h"
#include <QSet>

class QJsonArray;

class ProtonProvider final : public BackupProvider
{
public:
    explicit ProtonProvider(ProcessRunner &runner);

    void beginBackupOperation() override;
    void endBackupOperation() override;
    bool upload(const QString &localPath, const QString &remotePath, QString *error = nullptr) override;
    bool uploadImmutable(const QString &localPath, const QString &remotePath, QString *error = nullptr) override;
    RemoteLookup lookup(const QString &remotePath, QString *error = nullptr) override;
    bool supportsDirectoryUpload() const override { return true; }
    bool uploadDirectory(const QString &localPath, const QString &remotePath, QString *error = nullptr) override;
    bool ensureDirectory(const QString &remotePath, QString *error = nullptr) override;
    bool download(const QString &remotePath, const QString &localPath, QString *error = nullptr) override;
    bool inspect(const QString &remotePath, RemoteFile *file, QString *error = nullptr) override;
    bool inspectDirectoryFiles(const QString &remotePath, QVector<RemoteFile> *files, QString *error = nullptr) override;
    bool list(const QString &remotePath, QVector<RemoteItem> *items, QString *error = nullptr) override;
    bool trash(const QString &remotePath, QString *error = nullptr) override;
    bool permanentlyDelete(const QString &remotePath, QString *error = nullptr) override;

private:
    bool run(const QStringList &arguments, QString *error) const;
    bool listing(const QString &remotePath, QJsonArray *values, QString *error);
    ProcessRunner &runner;
    bool backupOperation = false;
    QSet<QString> ensuredDirectories;
};

#pragma once

#include "backupprovider.h"

class LocalProvider final : public BackupProvider
{
public:
    explicit LocalProvider(QString rootPath);

    bool upload(const QString &localPath, const QString &remotePath, QString *error = nullptr) override;
    bool uploadImmutable(const QString &localPath, const QString &remotePath, QString *error = nullptr) override;
    RemoteLookup lookup(const QString &remotePath, QString *error = nullptr) override;
    bool trustworthyContentChecksums() const override { return true; }
    bool ensureDirectory(const QString &remotePath, QString *error = nullptr) override;
    bool download(const QString &remotePath, const QString &localPath, QString *error = nullptr) override;
    bool inspect(const QString &remotePath, RemoteFile *file, QString *error = nullptr) override;
    bool list(const QString &remotePath, QVector<RemoteItem> *items, QString *error = nullptr) override;
    bool trash(const QString &remotePath, QString *error = nullptr) override;
    bool permanentlyDelete(const QString &remotePath, QString *error = nullptr) override;

private:
    QString rootPath;
};

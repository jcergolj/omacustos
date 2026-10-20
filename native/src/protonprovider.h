#pragma once

#include "backupprovider.h"
#include "processrunner.h"
#include <QSet>

class QJsonArray;
class QJsonObject;

class ProtonProvider final : public BackupProvider
{
public:
    bool listComplete(const QString &, QVector<RemoteItem> *, QString * = nullptr) override;
    bool deletionIdentity(const QString &, QString *, QString * = nullptr) override;
    bool trashExact(const QString &, const QString &, QString * = nullptr) override;
    bool deleteExact(const QString &, const QString &, QString * = nullptr) override;
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
    bool containedNode(const QString &, QJsonObject *, QString *);
    bool containedChildren(const QString &, const QString &, QJsonArray *, QString *);
    bool trashTarget(const QString &, QString *, bool *, QString *);
    bool exactMutation(const QString &, const QString &, const QString &, QString *);
    bool run(const QStringList &arguments, QString *error) const;
    bool listing(const QString &remotePath, QJsonArray *values, QString *error);
    ProcessRunner &runner;
    bool backupOperation = false;
    QSet<QString> ensuredDirectories;
};

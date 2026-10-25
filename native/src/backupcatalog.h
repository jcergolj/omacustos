#pragma once

#include "backupengine.h"
#include "backupprovider.h"

#include <QDateTime>
#include <QStringList>
#include <QVector>
#include <functional>

struct RemoteCopy {
    QString rootPath;
    QString manifestPath;
    QString computerName;
    QString setId;
    QString setName;
    QString copyId;
    QString status;
    QDateTime createdAt;
    QVector<BackupEntry> entries;
    QStringList unavailableItems;
    QStringList failedItems;
    int manifestVersion = 0;

    bool complete() const
    {
        return status == QStringLiteral("complete") && unavailableItems.isEmpty() && failedItems.isEmpty();
    }
};

class BackupCatalog final
{
public:
    static bool listLegacyHistory(BackupProvider &provider, const QString &remoteRoot, QVector<RemoteCopy> *copies,
        QString *error = nullptr, const std::function<bool()> &cancelled = {});
    static bool discover(BackupProvider &provider, const QString &remoteRoot, QVector<RemoteCopy> *copies, QString *error = nullptr,
        const std::function<bool()> &cancelled = {});
    // Identity/manifest discovery only; payloads are verified when a copy is selected.
    // With a shared set ID, lists only that set's incremental snapshots, not legacy copies.
    static bool listHistory(BackupProvider &provider, const QString &remoteRoot, QVector<RemoteCopy> *copies, QString *error = nullptr,
        const std::function<bool()> &cancelled = {}, const QString &expectedSharedSetId = {});
    static bool listCopies(BackupProvider &provider, const QString &backupFolder, QVector<RemoteCopy> *copies, QString *error = nullptr);
    static bool discoverCopies(BackupProvider &provider, const QString &backupFolder, const QString &expectedSetId,
        QVector<RemoteCopy> *copies, QString *error = nullptr);
    static bool verifyCopy(BackupProvider &provider, const QString &copyFolder, const QString &expectedSetId,
        RemoteCopy *copy, QString *error = nullptr, const std::function<bool()> &cancelled = {});
};

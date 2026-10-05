#pragma once

#include "backupcatalog.h"

#include <QHash>
#include <QStringList>

class QLockFile;

struct CleanupState {
    QString decision = QStringLiteral("pending");
    QStringList targets;
    QStringList trashed;
    QStringList completed;
    QString lastError;
    QString proposalId;
};

class CleanupStore final
{
public:
    explicit CleanupStore(QString path);

    bool load(QString *error = nullptr, QByteArray *contents = nullptr);
    bool loadFromBytes(const QByteArray &contents, QString *error = nullptr);
    QByteArray toBytes() const;
    QString filePath() const;
    CleanupState state(const QString &setId) const;
    bool propose(const QString &setId, const QStringList &targets, QString *error = nullptr);
    bool confirm(const QString &setId, const CleanupState &presented, QString *error = nullptr);
    bool forgetTarget(const QString &setId, const QString &target, QString *error = nullptr);

private:
    friend class BackupCleanup;

    bool lockAndLoad(QLockFile &lock, QString *error);
    bool save(QString *error = nullptr) const;
    QString path;
    QHash<QString, CleanupState> cleanupStates;
};

class BackupCleanup final
{
public:
    static QStringList eligibleTargets(const QVector<RemoteCopy> &copies, int retention,
        const QString &computerName = {}, const QString &setId = {});
    static bool run(BackupProvider &provider, CleanupStore &store, const QString &setId,
        const QStringList &targets, const QString &allowedRoot, QString *error = nullptr);
    static bool apply(BackupProvider &provider, CleanupStore &store, const QString &setId,
        const QString &allowedRoot = {}, QString *error = nullptr);
    static bool apply(BackupProvider &provider, CleanupStore &store, const QString &setId, QString *error)
    {
        return apply(provider, store, setId, {}, error);
    }
};

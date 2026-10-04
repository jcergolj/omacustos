#pragma once

#include "backupcatalog.h"

#include <QHash>
#include <QStringList>

struct CleanupState {
    QString decision = QStringLiteral("pending");
    QStringList targets;
    QStringList trashed;
    QStringList completed;
    QString lastError;
};

class CleanupStore final
{
public:
    explicit CleanupStore(QString path);

    bool load(QString *error = nullptr, QByteArray *contents = nullptr);
    bool loadFromBytes(const QByteArray &contents, QString *error = nullptr);
    bool save(QString *error = nullptr) const;
    CleanupState state(const QString &setId) const;
    void setPending(const QString &setId, const QStringList &targets);
    void setTargets(const QString &setId, const QStringList &targets);
    void confirm(const QString &setId);
    QString filePath() const;
    QHash<QString, CleanupState> &states();

private:
    QString path;
    QHash<QString, CleanupState> cleanupStates;
};

class BackupCleanup final
{
public:
    static QStringList eligibleTargets(const QVector<RemoteCopy> &copies, int retention,
        const QString &computerName = {}, const QString &setId = {});
    static bool apply(BackupProvider &provider, CleanupStore &store, const QString &setId,
        const QString &allowedRoot = {}, QString *error = nullptr);
    static bool apply(BackupProvider &provider, CleanupStore &store, const QString &setId, QString *error)
    {
        return apply(provider, store, setId, {}, error);
    }
};

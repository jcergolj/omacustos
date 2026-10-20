#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>

struct RemoteFile {
    QString path;
    qint64 size = 0;
    QByteArray checksum;
    // Optional immutable content revision, not a node ID or display timestamp.
    QString revision;
};

struct RemoteItem {
    QString path;
    QString name;
    bool directory = false;
    qint64 size = 0;
    QDateTime modified;
};

enum class RemoteLookup { Found, Missing, Unknown };

class BackupProvider
{
public:
    virtual ~BackupProvider() = default;

    // Scope provider caches to one synchronous backup, including early failures.
    virtual void beginBackupOperation() {}
    virtual void endBackupOperation() {}

    virtual bool upload(const QString &localPath, const QString &remotePath, QString *error = nullptr) = 0;
    // Incremental storage requires conflict-failing writes and explicit absence.
    // Defaults fail closed; legacy providers/callers keep their existing behavior.
    virtual bool uploadImmutable(const QString &, const QString &, QString * = nullptr) { return false; }
    virtual RemoteLookup lookup(const QString &, QString * = nullptr) { return RemoteLookup::Unknown; }
    virtual bool trustworthyContentChecksums() const { return false; }
    // Optional recursive upload of a prepared tree into a fresh copy namespace.
    // The local directory basename must match the remote directory basename.
    virtual bool supportsDirectoryUpload() const { return false; }
    virtual bool uploadDirectory(const QString &, const QString &, QString * = nullptr) { return false; }
    virtual bool ensureDirectory(const QString &remotePath, QString *error = nullptr) = 0;
    virtual bool download(const QString &remotePath, const QString &localPath, QString *error = nullptr) = 0;
    virtual bool inspect(const QString &remotePath, RemoteFile *file, QString *error = nullptr) = 0;
    // Optional bulk content metadata. False means unsupported/unavailable;
    // omitted files must still be inspected individually by the caller.
    virtual bool inspectDirectoryFiles(const QString &, QVector<RemoteFile> *, QString * = nullptr) { return false; }
    virtual bool list(const QString &remotePath, QVector<RemoteItem> *items, QString *error = nullptr) = 0;
    virtual bool trash(const QString &remotePath, QString *error = nullptr) = 0;
    virtual bool permanentlyDelete(const QString &remotePath, QString *error = nullptr) = 0;
    // GC capabilities are opt-in. Complete enumeration must not silently omit
    // unreadable/ambiguous children. Exact deletion uses a persisted stable node
    // identity, reconciles trash, and is idempotent after a lost response.
    virtual bool listComplete(const QString &, QVector<RemoteItem> *, QString * = nullptr) { return false; }
    virtual bool deletionIdentity(const QString &, QString *, QString * = nullptr) { return false; }
    virtual bool trashExact(const QString &, const QString &, QString * = nullptr) { return false; }
    virtual bool deleteExact(const QString &, const QString &, QString * = nullptr) { return false; }
};

#pragma once

#include "backupresult.h"

#include <QByteArray>
#include <QString>
#include <QDateTime>
#include <QStringList>
#include <QVector>

struct BackupArchive {
    QString id;
    QString remotePath;
    qint64 size = 0;
    QByteArray checksum;
    QStringList members; // Derived from the validated index, never independently persisted.
};

struct BackupEntry {
    QString sourcePath;
    QString remotePath;
    qint64 size = 0;
    QByteArray checksum;
    QString restorePath;
    BackupArchive archive;
    QString memberPath;
};

struct BackupCopyMetadata {
    QString computerName;
    QString setId;
    QString setName;
    QString copyId;
    QDateTime createdAt;
};

// Execution supplies observations, never a format version or completion claim.
// Only payloads that passed provider verification belong in verifiedEntries.
struct BackupManifestDraft {
    BackupCopyMetadata metadata;
    QVector<BackupEntry> verifiedEntries;
    QStringList expectedItems;
    QStringList failedItems;
    QVector<BackupIssue> issues;
    QVector<BackupArchive> archives;
};

struct BackupManifestInfo {
    int version = 0;
    QString application;
    QString computerName;
    QString setId;
    QString setName;
    QString copyId;
    QDateTime createdAt;
    QString status;
    QStringList expectedItems;
    QStringList failedItems;
    QVector<BackupIssue> issues;
};

class BackupManifest
{
public:
    static bool write(const QString &path, const BackupManifestDraft &draft, QString *error = nullptr);
    static bool load(const QString &path, QVector<BackupEntry> *entries, QString *error = nullptr);
    static bool load(const QString &path, QVector<BackupEntry> *entries, BackupManifestInfo *info, QString *error = nullptr);
    // V1 has no provenance. Compatibility derives identity only from a scoped,
    // timestamped copy folder with all validated payload references inside it.
    static bool identifyLegacyCopy(const QString &copyFolder, const QString &setId,
        const QVector<BackupEntry> &entries, BackupManifestInfo *info);
};

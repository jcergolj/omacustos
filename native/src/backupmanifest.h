#pragma once

#include "backupresult.h"

#include <QByteArray>
#include <QString>
#include <QDateTime>
#include <QStringList>
#include <QVector>

struct BackupEntry {
    QString sourcePath;
    QString remotePath;
    qint64 size = 0;
    QByteArray checksum;
    QString restorePath;
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
};

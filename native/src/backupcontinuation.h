#pragma once

#include "backupmanifest.h"
#include <QHash>
#include <QMap>

// A header fixes copy identity and root mappings. Each atomic batch file is an
// independently committed checkpoint; publishing a batch never rewrites the
// entire history. This is recovery state, never a restorable manifest.
class BackupContinuation final
{
public:
    explicit BackupContinuation(QString directory);
    bool open(const QString &remoteRoot, const BackupCopyMetadata &metadata, QString *error);
    bool saveRoots(QString *error);
    bool checkpoint(const QVector<BackupEntry> &entries, QString *error, bool verifiedPayloads = true);
    QHash<QString, BackupEntry> verified;
    QHash<QString, BackupEntry> mappings;
    QMap<QString, QString> roots;
    BackupCopyMetadata metadata;

private:
    QString directory;
    QString remoteRoot;
    int nextBatch = 0;
};

#pragma once

#include "backupcatalog.h"

// Read-only recovery needs no local identity, configuration or journals.
class IncrementalRestore
{
public:
    static bool snapshotLocation(const QString &snapshotRoot, QString *setRoot, QString *setId, QString *snapshotId);
    static bool describeCopy(BackupProvider &provider, const QString &snapshotRoot, const QString &expectedSetId,
        RemoteCopy *copy, QString *error, const std::function<bool()> &cancelled = {});
    // Selected-set metadata only: one listing, one anchor, one manifest per snapshot.
    static bool listSnapshots(BackupProvider &provider, const QString &remoteRoot, const QString &setId,
        QVector<RemoteCopy> *copies, QString *error, const std::function<bool()> &cancelled = {});
    // Display-only candidates from one directory listing. A listed folder may
    // be incomplete; only verifyCopy can make its files available for restore.
    static bool listSnapshotCandidates(BackupProvider &provider, const QString &remoteRoot, const QString &setId,
        QVector<RemoteCopy> *copies, QString *error, const std::function<bool()> &cancelled = {});
    static bool verifyCopy(BackupProvider &provider, const QString &snapshotRoot, const QString &expectedSetId,
        RemoteCopy *copy, QString *error, const std::function<bool()> &cancelled = {});
    static bool discover(BackupProvider &provider, const QString &remoteRoot, QVector<RemoteCopy> *copies, QString *error);
};

#pragma once

#include "backupcatalog.h"

// Read-only recovery needs no local identity, configuration or journals.
class IncrementalRestore
{
public:
    static bool snapshotLocation(const QString &snapshotRoot, QString *setRoot, QString *setId, QString *snapshotId);
    static bool verifyCopy(BackupProvider &provider, const QString &snapshotRoot, const QString &expectedSetId,
        RemoteCopy *copy, QString *error, const std::function<bool()> &cancelled = {});
    static bool discover(BackupProvider &provider, const QString &remoteRoot, QVector<RemoteCopy> *copies, QString *error);
};

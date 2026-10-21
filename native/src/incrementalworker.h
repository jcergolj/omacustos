#pragma once

#include "backupconfig.h"
#include "backupengine.h"
#include "incrementalrepository.h"

// Normal-worker adapter. Local identity is installation-bound; remote ownership
// is still the manual-offline protocol, never an online lease/fencing claim.
class IncrementalWorker
{
public:
    static QString setId(const BackupSet &set);
    static QString root(const BackupSet &set);
    static bool request(const BackupSet &set, const QString &stateDirectory,
        const QString &snapshotId, BackupProvider &provider, IncrementalWriteRequest *request,
        QString *error, bool provision = true,
        const std::function<bool(const QString &, QString *)> &stagingReady = {});
    static IncrementalWriteResult run(const BackupSet &set, IncrementalWriteRequest request,
        BackupProvider &provider, const std::function<IncrementalControl(const QString &)> &control = {},
        const std::function<void(const QString &)> &warning = {}, const QString &configPath = {});
    static void excludeCoordinationFiles(BackupPreview *selection, const QString &configPath);
    static bool takeover(const BackupSet &set, const QString &stateDirectory, BackupProvider &provider,
        bool oldWriterDisabled, QString *error);
};

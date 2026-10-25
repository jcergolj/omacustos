#pragma once

#include "backupconfig.h"
#include "backupprovider.h"
#include <QVariantList>

// Manual, append-only settings versions. No backup/ownership operations belong here.
class SettingsArchive final
{
public:
    static QString root() { return QStringLiteral("/my-files/omacustos/settings"); }
    static bool exportSettings(BackupProvider &provider, const BackupConfig &config,
        const QString &computer, QString *remotePath, QString *error);
    static bool browse(BackupProvider &provider, QVariantList *versions, QString *error);
    static bool download(BackupProvider &provider, const QString &remotePath,
        const QString &localPath, QString *error);
};

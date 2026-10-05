#pragma once

#include <QString>

class BackupPrerequisiteProbe;

// Shared executable path; setup supplies the system prerequisite adapter.
int runBackupWorker(const QString &configPath, BackupPrerequisiteProbe &prerequisites);

#include "backupprerequisites.h"

#include <QDir>
#include <QFile>

bool SystemBackupPrerequisiteProbe::onAcPower() const
{
    const QDir powerSupply(QStringLiteral("/sys/class/power_supply"));
    const QStringList entries = powerSupply.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    bool foundPowerSource = false;
    for (const QString &entry : entries) {
        if (!entry.startsWith(QStringLiteral("AC")) && !entry.startsWith(QStringLiteral("ADP"))) {
            continue;
        }

        foundPowerSource = true;
        QFile online(powerSupply.filePath(entry + QStringLiteral("/online")));
        if (online.open(QIODevice::ReadOnly) && online.readAll().trimmed() == QByteArray("1")) {
            return true;
        }
    }

    return !foundPowerSource;
}

BackupPrerequisiteResult BackupPrerequisites::check(const BackupSet &set, const BackupPrerequisiteProbe &probe)
{
    if (set.schedule.paused) {
        return {false, QStringLiteral("Imported settings are paused. Correct source paths, complete offline takeover, then explicitly resume this set. Restore remains available.")};
    }
    if (set.onlyOnAcPower && !probe.onAcPower()) {
        return {false, QStringLiteral("Waiting for AC power.")};
    }

    return {};
}

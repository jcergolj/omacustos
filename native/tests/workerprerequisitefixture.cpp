#include "backupprerequisites.h"
#include "workerexecution.h"

#include <QCoreApplication>
#include <QCommandLineParser>

class BatteryProbe final : public BackupPrerequisiteProbe
{
public:
    bool onAcPower() const override { return false; }
};

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addOption({"config", "Configuration file.", "path"});
    parser.process(application);
    BatteryProbe prerequisites;
    return runBackupWorker(parser.value("config"), prerequisites);
}

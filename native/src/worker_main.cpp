#include "backupprerequisites.h"
#include "workerexecution.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({{"c", "config"}, QStringLiteral("Configuration file."), QStringLiteral("path")});
    parser.process(application);

    const QString configPath = parser.value(QStringLiteral("config")).isEmpty()
        ? QDir::home().filePath(QStringLiteral(".config/omacustos/omacustos-backup.json"))
        : parser.value(QStringLiteral("config"));
    SystemBackupPrerequisiteProbe prerequisites;
    return runBackupWorker(configPath, prerequisites);
}

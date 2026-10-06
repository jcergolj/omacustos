#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include "backupengine.h"
#include "backupconfig.h"
#include "backuplauncher.h"
#include "backuprestorecontroller.h"
#include "backupsetcontroller.h"
#include "protonprovider.h"
#include "protonfolderbrowser.h"
#include "recentbackupcopies.h"
#include "qprocessrunner.h"
#include "protonauthcontroller.h"
#include "themecolors.h"
#include "resourceusage.h"
#include "backupscheduler.h"
#include <QDir>
#include <QSysInfo>
#include <QTimer>

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("omacustos"));
    application.setApplicationDisplayName(QStringLiteral("OmaCustos for Proton Drive"));
    application.setDesktopFileName(QStringLiteral("omacustos"));
    QQmlApplicationEngine engine;
    BackupEngine backupEngine;
    BackupLauncher backupLauncher;
    BackupSetController backupSetController(
        backupEngine,
        QDir::home().filePath(QStringLiteral(".config/omacustos/omacustos-backup.json"))
    );
    BackupConfig config;
    QString configError;
    const QString configPath = QDir::home().filePath(QStringLiteral(".config/omacustos/omacustos-backup.json"));
    const QString protonBinary = BackupConfigStore(configPath).load(&config, &configError)
        ? config.protonBinary
        : qEnvironmentVariable("OMACUSTOS_PROTON_BIN", QStringLiteral("proton-drive"));
    QProcessRunner browserRunner(
        protonBinary
    );
    ProtonProvider browserProvider(browserRunner);
    ProtonFolderBrowser protonFolderBrowser(browserRunner, ProtonFolderLink::cachePath(configPath));
    RecentBackupCopies recentBackupCopies(browserProvider, configPath, QSysInfo::machineHostName());
    QProcessRunner restoreRunner(protonBinary);
    ProtonProvider restoreProvider(restoreRunner);
    BackupRestoreController restoreController(backupEngine, &restoreProvider);
    restoreRunner.setStopRequested([&restoreController] { return restoreController.stopRequested(); });
    ProtonAuthController protonAuth(protonBinary);
    ThemeColors themeColors;
    ResourceUsage resourceUsage;
    BackupScheduler backupScheduler(configPath);
    const auto prefetchBrowserLinks = [&] {
        protonFolderBrowser.prefetchFolders(backupSetController.recentBackupCopyPaths());
    };
    QObject::connect(&backupSetController, &BackupSetController::dashboardChanged,
        &protonFolderBrowser, prefetchBrowserLinks);
    bool browserLinksConnected = false;
    QObject::connect(&protonAuth, &ProtonAuthController::stateChanged, &protonFolderBrowser, [&] {
        const bool connected = protonAuth.authenticated();
        if (connected && !browserLinksConnected) {
            protonFolderBrowser.prefetchFolders(backupSetController.recentBackupCopyPaths(), true);
        }
        browserLinksConnected = connected;
    });
    prefetchBrowserLinks();
    bool schedulingUpdatePending = false;
    const auto updateScheduling = [&] {
        if (resourceUsage.busy()) {
            schedulingUpdatePending = true;
        } else {
            schedulingUpdatePending = false;
            backupScheduler.applySavedSchedules();
        }
    };
    QObject::connect(&backupSetController, &BackupSetController::configurationSaved, &backupScheduler, [&] {
        // The Save handler also applies resources. Let it finish writing/reloading
        // them before starting a timer that may immediately launch overdue work.
        QTimer::singleShot(0, &backupScheduler, updateScheduling);
    });
    QObject::connect(&resourceUsage, &ResourceUsage::busyChanged, &backupScheduler, [&] {
        if (!resourceUsage.busy() && schedulingUpdatePending) {
            updateScheduling();
        }
    });

    engine.rootContext()->setContextProperty(QStringLiteral("backupEngine"), &backupEngine);
    engine.rootContext()->setContextProperty(QStringLiteral("backupLauncher"), &backupLauncher);
    engine.rootContext()->setContextProperty(QStringLiteral("backupSetController"), &backupSetController);
    engine.rootContext()->setContextProperty(QStringLiteral("restoreController"), &restoreController);
    engine.rootContext()->setContextProperty(QStringLiteral("protonFolderBrowser"), &protonFolderBrowser);
    engine.rootContext()->setContextProperty(QStringLiteral("recentBackupCopies"), &recentBackupCopies);
    engine.rootContext()->setContextProperty(QStringLiteral("protonAuth"), &protonAuth);
    engine.rootContext()->setContextProperty(QStringLiteral("themeColors"), &themeColors);
    engine.rootContext()->setContextProperty(QStringLiteral("resourceUsage"), &resourceUsage);
    engine.rootContext()->setContextProperty(QStringLiteral("backupScheduler"), &backupScheduler);
    engine.loadFromModule(QStringLiteral("OmaCustos"), QStringLiteral("Main"));

    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    return application.exec();
}

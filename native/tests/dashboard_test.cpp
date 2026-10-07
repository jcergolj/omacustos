#include <QGuiApplication>
#include <QDesktopServices>
#include <QFile>
#include <QDir>
#include <QPalette>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtQuickTest>

class DashboardSetup final : public QObject
{
    Q_OBJECT

public:
    DashboardSetup()
    {
        if (!home.isValid()) {
            qFatal("Cannot create an isolated home for dashboard tests.");
        }
        qputenv("HOME", home.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", home.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", home.filePath("cache").toUtf8());
        QStandardPaths::setTestModeEnabled(true);
    }

    Q_INVOKABLE QString openedDocumentationUrl() const
    {
        return documentationUrl.toString();
    }

public slots:
    void openDocumentation(const QUrl &url)
    {
        documentationUrl = url;
    }

    void applicationAvailable()
    {
        QDesktopServices::setUrlHandler(QStringLiteral("https"), this, "openDocumentation");
        QPalette darkPalette;
        darkPalette.setColor(QPalette::Window, QColor("#12161c"));
        darkPalette.setColor(QPalette::WindowText, Qt::white);
        QGuiApplication::setPalette(darkPalette);
    }

    void qmlEngineAvailable(QQmlEngine *engine)
    {
        engine->rootContext()->setContextProperty(QStringLiteral("dashboardBrowser"), this);
        const QString importPath = home.filePath(QStringLiteral("imported sets.json"));
        const QString invalidPath = home.filePath(QStringLiteral("invalid sets.json"));
        for (const QString &path : {importPath, invalidPath}) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) {
                qFatal("Cannot create import-dialog test fixture.");
            }
            file.write(path == importPath ? R"({"application":"omacustos","version":1,"sets":[]})" : "{");
        }
        engine->rootContext()->setContextProperty(QStringLiteral("dashboardImportFileUrl"), QUrl::fromLocalFile(importPath));
        engine->rootContext()->setContextProperty(QStringLiteral("dashboardImportFilePath"), importPath);
        engine->rootContext()->setContextProperty(QStringLiteral("dashboardInvalidImportFileUrl"), QUrl::fromLocalFile(invalidPath));
        const QString restorePath = home.filePath(QStringLiteral("restored files"));
        if (!QDir().mkpath(restorePath)) {
            qFatal("Cannot create restore destination test fixture.");
        }
        engine->rootContext()->setContextProperty(QStringLiteral("dashboardRestoreFolderUrl"), QUrl::fromLocalFile(restorePath));
        engine->rootContext()->setContextProperty(QStringLiteral("dashboardRestoreFolderPath"), restorePath);
        QQmlComponent component(engine, QUrl::fromLocalFile(QStringLiteral(QUICK_TEST_SOURCE_DIR "/DashboardControllers.qml")));
        QObject *controllers = component.create();
        if (!controllers) {
            qFatal("Cannot load dashboard controller doubles: %s", qPrintable(component.errorString()));
        }
        controllers->setParent(engine);
        for (const char *name : {"backupSetController", "backupLauncher", "restoreController", "protonFolderBrowser", "recentBackupCopies", "protonAuth", "themeColors", "resourceUsage", "backupScheduler"}) {
            engine->rootContext()->setContextProperty(QString::fromLatin1(name), controllers->property(name).value<QObject *>());
        }
        engine->rootContext()->setContextProperty(QStringLiteral("dashboardScreenshotPath"), qEnvironmentVariable("OMACUSTOS_TEST_SCREENSHOT"));
    }

private:
    QUrl documentationUrl;
    QTemporaryDir home;
};

QUICK_TEST_MAIN_WITH_SETUP(dashboard, DashboardSetup)
#include "dashboard_test.moc"

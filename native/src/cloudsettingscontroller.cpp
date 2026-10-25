#include "cloudsettingscontroller.h"
#include "settingsarchive.h"
#include <QSysInfo>
#include <QUuid>
#include <QtConcurrentRun>

CloudSettingsController::CloudSettingsController(BackupProvider &provider, QString path, QObject *parent)
    : QObject(parent), provider(provider), configPath(std::move(path))
{
    connect(&watcher, &QFutureWatcher<QVariantMap>::finished, this, [this] {
        const auto result = watcher.result();
        working = false;
        lastError = result.value("error").toString();
        if (result.contains("versions")) rows = result.value("versions").toList();
        emit changed();
        if (!lastError.isEmpty()) return;
        if (result.contains("download")) emit downloaded(result.value("download").toString());
        if (result.contains("export")) emit exported(result.value("export").toString());
    });
}
CloudSettingsController::~CloudSettingsController() { stopped.store(true); watcher.waitForFinished(); }
void CloudSettingsController::browse() { start("browse"); }
void CloudSettingsController::exportSettings() { start("export"); }
void CloudSettingsController::previewVersion(const QString &path) { start("download", path); }
void CloudSettingsController::start(const QString &action, const QString &selection)
{
    if (working) return;
    working = true;
    lastError.clear();
    if (action == "browse") rows.clear();
    emit changed();
    const auto path = configPath;
    const auto local = workspace.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json");
    auto *remote = &provider;
    watcher.setFuture(QtConcurrent::run([remote, path, action, selection, local] {
        QString error;
        QVariantMap result;
        if (action == "browse") {
            QVariantList versions;
            if (SettingsArchive::browse(*remote, &versions, &error)) result.insert("versions", versions);
        } else if (action == "export") {
            BackupConfig config;
            QString uploaded;
            if (BackupConfigStore(path).load(&config, &error)
                && SettingsArchive::exportSettings(*remote, config, QSysInfo::machineHostName(), &uploaded, &error)) result.insert("export", uploaded);
        } else if (SettingsArchive::download(*remote, selection, local, &error)) result.insert("download", local);
        if (result.isEmpty() && error.isEmpty()) error = "The settings operation failed.";
        result.insert("error", error);
        return result;
    }));
}

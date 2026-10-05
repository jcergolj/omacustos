#pragma once

#include "backupprovider.h"
#include "backupprogress.h"
#include "backupresult.h"
#include "backupmanifest.h"

#include <QObject>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <functional>

struct BackupPreview {
    QStringList includedFiles;
    QStringList excludedFiles;
    QStringList skippedPaths;
    QStringList missingPaths;
};

struct BackupOptions {
    // New copy namespaces skip reuse checks and verify payloads together after
    // uploads finish, using folder metadata with individual inspection fallback.
    // Providers supporting recursive upload receive a staged tree in one call
    // when a folder source is selected; its staging is removed before verification.
    bool freshCopy = false;
};

class BackupEngine final : public QObject
{
    Q_OBJECT

public:
    explicit BackupEngine(QObject *parent = nullptr);

    Q_INVOKABLE bool validateSelection(const QString &sourceDirectory, QString *error = nullptr) const;
    Q_INVOKABLE QStringList selectableFiles(const QString &sourceDirectory) const;
    Q_INVOKABLE QStringList selectableFiles(const QStringList &sourceDirectories, const QStringList &exclusions) const;
    Q_INVOKABLE QVariantMap previewSelection(const QStringList &sourceDirectories, const QStringList &exclusions) const;
    Q_INVOKABLE QString previewError(const QString &sourceDirectory) const;
    bool backup(const QString &sourceDirectory, const QString &remoteRoot, BackupProvider &provider, QString *manifestPath, QString *error = nullptr) const;
    bool backup(const QStringList &sourceDirectories, const QString &remoteRoot, const QStringList &exclusions, BackupProvider &provider, QString *manifestPath, QString *error = nullptr) const;
    bool backup(const QStringList &sourceDirectories, const QString &remoteRoot, const QStringList &exclusions, const BackupCopyMetadata &metadata, BackupProvider &provider, QString *manifestPath, QString *error = nullptr, const std::function<void(const BackupProgress &)> &reportProgress = {}, BackupResult *result = nullptr, const BackupOptions &options = {}) const;
    bool restoreFile(const BackupEntry &entry, const QString &destinationDirectory, BackupProvider &provider, QString *error = nullptr) const;

    BackupPreview preview(const QStringList &sourceDirectories, const QStringList &exclusions, const std::function<bool()> &cancelled = {}) const;

private:
    BackupPreview scan(const QStringList &sourceDirectories, const QStringList &exclusions, bool reportExcluded, const std::function<bool()> &cancelled = {}) const;
};

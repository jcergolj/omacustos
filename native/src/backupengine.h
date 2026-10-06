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
    qint64 totalBytes = 0;
};

struct BackupOptions {
    bool freshCopy = false;
    bool singleArchive = false; // Internal execution seam; workers use bounded archives.
    bool boundedArchives = false;
    qint64 archiveTargetBytes = 1000 * 1000 * 1000;
    QString stagingDirectory;
    qint64 stagingBudget = 1000 * 1000 * 1000;
    int batchFileLimit = 1000;
    QString continuationDirectory;
    std::function<bool()> stopped;
    bool retainLocalManifest = true;
    std::function<bool(const QString &, QString *)> stagingReady;
};

struct BackupRestoreRequest {
    QVector<BackupEntry> entries;
    QString destinationDirectory;
    QString copyPath;
};

struct BackupRestoreResult {
    int restoredCount = 0;
    QString error;
    bool success = false;
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
    BackupRestoreResult restoreFiles(const BackupRestoreRequest &request, BackupProvider &provider,
        const std::function<void(int)> &reportProgress = {}) const;

    BackupPreview preview(const QStringList &sourceDirectories, const QStringList &exclusions, const std::function<bool()> &cancelled = {}) const;

private:
    bool restoreEntry(const BackupEntry &entry, const QString &destinationDirectory, BackupProvider &provider, QString *error) const;
    bool backupBatches(const QStringList &sources, const QString &remoteRoot, const QStringList &exclusions,
        const BackupCopyMetadata &metadata, BackupProvider &provider, QString *manifestPath, QString *error,
        const std::function<void(const BackupProgress &)> &reportProgress, BackupResult *result, const BackupOptions &options) const;
    BackupPreview scan(const QStringList &sourceDirectories, const QStringList &exclusions, bool reportExcluded, const std::function<bool()> &cancelled = {}) const;
};

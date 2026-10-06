#pragma once

#include "backupengine.h"
#include "backupcatalog.h"

#include <QObject>
#include <QFutureWatcher>
#include <QStringList>
#include <QVariantList>
#include <QVector>
#include <QCache>
#include <QThreadPool>
#include <atomic>
#include <memory>

class BackupRestoreController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList entries READ entries NOTIFY entriesChanged)
    Q_PROPERTY(QString defaultDestination READ defaultDestination CONSTANT)
    Q_PROPERTY(QStringList copies READ copies NOTIFY copiesChanged)
    Q_PROPERTY(QString copySearch READ copySearch WRITE setCopySearch NOTIFY copiesChanged)
    Q_PROPERTY(QStringList unavailableEntries READ unavailableEntries NOTIFY entriesChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool browsing READ browsing NOTIFY busyChanged)
    Q_PROPERTY(bool restoring READ isRestoring NOTIFY busyChanged)
    Q_PROPERTY(QString backupFolder READ backupFolder NOTIFY copiesChanged)
    Q_PROPERTY(QString backupId READ backupId NOTIFY copiesChanged)
    Q_PROPERTY(QString currentCopyPath READ currentCopyPath NOTIFY currentCopyIndexChanged)
    Q_PROPERTY(QString browseError READ browseError NOTIFY busyChanged)
    Q_PROPERTY(QString restoreProgress READ restoreProgress NOTIFY restoreProgressChanged)
    Q_PROPERTY(double restoreProgressFraction READ restoreProgressFraction NOTIFY restoreProgressChanged)
    Q_PROPERTY(QString restoreBackupFolder READ restoreBackupFolder NOTIFY restoreProgressChanged)
    Q_PROPERTY(QString restoreBackupId READ restoreBackupId NOTIFY restoreProgressChanged)
    Q_PROPERTY(QString restoreCopyPath READ restoreCopyPath NOTIFY restoreProgressChanged)
    Q_PROPERTY(QString loadingMessage READ loadingMessage NOTIFY busyChanged)
    Q_PROPERTY(bool showingCachedData READ showingCachedData NOTIFY cachedDataChanged)
    Q_PROPERTY(bool restoreEligible READ restoreEligible NOTIFY restoreEligibilityChanged)
    Q_PROPERTY(int currentCopyIndex READ currentCopyIndex NOTIFY currentCopyIndexChanged)

public:
    explicit BackupRestoreController(BackupEngine &engine, BackupProvider *provider = nullptr, QObject *parent = nullptr);
    ~BackupRestoreController() override;

    QStringList entries() const;
    QString defaultDestination() const;
    QStringList copies() const;
    QString copySearch() const;
    void setCopySearch(const QString &search);
    QStringList unavailableEntries() const;
    Q_INVOKABLE void loadManifest(const QString &path);
    bool busy() const;
    bool browsing() const { return loading; }
    bool isRestoring() const { return restoring; }
    QString backupFolder() const { return activeBackupFolder; }
    QString backupId() const { return expectedSetId; }
    QString currentCopyPath() const;
    QString browseError() const { return refreshError; }
    QString restoreProgress() const;
    double restoreProgressFraction() const;
    QString restoreBackupFolder() const { return transferBackupFolder; }
    QString restoreBackupId() const { return transferBackupId; }
    QString restoreCopyPath() const { return transferCopyPath; }
    QString loadingMessage() const;
    bool showingCachedData() const;
    bool restoreEligible() const;
    int currentCopyIndex() const;
    Q_INVOKABLE void discover(const QString &backupFolder, const QString &setId = QString());
    Q_INVOKABLE void selectCopy(int index);
    Q_INVOKABLE void restore(int index, const QString &destinationDirectory);
    Q_INVOKABLE void restoreSelected(const QVariantList &indexes, const QString &destinationDirectory);
    Q_INVOKABLE void restoreFolder(const QString &folder, const QString &destinationDirectory);

signals:
    void busyChanged();
    void cachedDataChanged();
    void restoreEligibilityChanged();
    void currentCopyIndexChanged();
    void entriesChanged();
    void copiesChanged();
    void statusChanged(const QString &status);
    void failed(const QString &error);
    void restoreCompleted();
    void restoreCompletedForContext(const QString &backupFolder, const QString &setId, const QString &copyPath);
    void restoreProgressChanged();

private:
    struct BrowseResult {
        QVector<RemoteCopy> copies;
        RemoteCopy copy;
        QString error;
        bool success = false;
    };
    struct BrowseRequest {
        quint64 generation = 0;
        bool listing = false;
        QString folder;
        QString setId;
        QString copyPath;
        QString selectedPath;
    };
    struct Snapshot {
        QVector<RemoteCopy> copies;
        QVector<BackupEntry> entries;
        int selectedIndex = -1;
        QString search;
    };
    QFutureWatcher<BrowseResult> watcher;
    QFutureWatcher<BackupRestoreResult> restoreWatcher;
    // Provider/runner instances are not assumed to support simultaneous calls.
    // Navigation is immediate; metadata requests queue behind an active transfer.
    QThreadPool operations;
    QCache<QString, Snapshot> snapshots {20};
    BrowseRequest pendingBrowse;
    BrowseRequest activeBrowse;
    quint64 browseGeneration = 0;
    bool browseInFlight = false;
    std::shared_ptr<std::atomic_bool> browseCancelled;
    bool loading = false;
    bool restoring = false;
    bool verified = false;
    bool cachedData = false;
    bool discovering = false;
    QString expectedSetId;
    QString activeBackupFolder;
    QString refreshError;
    QString transferBackupFolder;
    QString transferBackupId;
    QString transferCopyPath;
    int transferTotal = 0;
    int transferred = 0;
    BackupEngine &engine;
    BackupProvider *provider;
    QVector<BackupEntry> manifestEntries;
    QStringList entryPaths;
    QVector<RemoteCopy> remoteCopies;
    int selectedCopyIndex = -1;

    QVector<int> filteredCopyIndexes() const;
    void setCachedData(bool cached);
    void startBrowse();
    void publishEntries();
    void startRestore(const QVector<BackupEntry> &entries, const QString &destination);
    QString searchText;
};

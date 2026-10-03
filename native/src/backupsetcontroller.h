#pragma once

#include "backupconfig.h"
#include "backupengine.h"
#include "backuprunstore.h"
#include "backupecleanup.h"

#include <QObject>
#include <QFutureWatcher>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <atomic>
#include <memory>

class BackupSetController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList setNames READ setNames NOTIFY setsChanged)
    Q_PROPERTY(QStringList setIds READ setIds NOTIFY setsChanged)
    Q_PROPERTY(QString currentId READ currentId NOTIFY currentSetChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(QString currentName READ currentName WRITE setCurrentName NOTIFY currentSetChanged)
    Q_PROPERTY(QString currentRemoteRoot READ currentRemoteRoot WRITE setCurrentRemoteRoot NOTIFY currentSetChanged)
    Q_PROPERTY(QStringList currentSources READ currentSources WRITE setCurrentSources NOTIFY currentSetChanged)
    Q_PROPERTY(QStringList currentExclusions READ currentExclusions WRITE setCurrentExclusions NOTIFY currentSetChanged)
    Q_PROPERTY(QString currentScheduleFrequency READ currentScheduleFrequency WRITE setCurrentScheduleFrequency NOTIFY currentSetChanged)
    Q_PROPERTY(int currentScheduleHour READ currentScheduleHour WRITE setCurrentScheduleHour NOTIFY currentSetChanged)
    Q_PROPERTY(int currentScheduleMinute READ currentScheduleMinute WRITE setCurrentScheduleMinute NOTIFY currentSetChanged)
    Q_PROPERTY(int currentScheduleWeekday READ currentScheduleWeekday WRITE setCurrentScheduleWeekday NOTIFY currentSetChanged)
    Q_PROPERTY(int currentScheduleDayOfMonth READ currentScheduleDayOfMonth WRITE setCurrentScheduleDayOfMonth NOTIFY currentSetChanged)
    Q_PROPERTY(int currentRetention READ currentRetention WRITE setCurrentRetention NOTIFY currentSetChanged)
    Q_PROPERTY(bool currentOnlyOnAcPower READ currentOnlyOnAcPower WRITE setCurrentOnlyOnAcPower NOTIFY currentSetChanged)
    Q_PROPERTY(QString currentNextRun READ currentNextRun NOTIFY currentSetChanged)
    Q_PROPERTY(QString currentRunStatus READ currentRunStatus NOTIFY runStateChanged)
    Q_PROPERTY(QString currentRunError READ currentRunError NOTIFY runStateChanged)
    Q_PROPERTY(QStringList runningSetIds READ runningSetIds NOTIFY runningSetIdsChanged)
    Q_PROPERTY(QVariantMap remainingTimes READ remainingTimes NOTIFY remainingTimesChanged)
    Q_PROPERTY(QVariantMap transferProgress READ transferProgress NOTIFY transferProgressChanged)
    Q_PROPERTY(QVariantMap runDetails READ runDetails NOTIFY runDetailsChanged)
    Q_PROPERTY(QVariantMap runSummaries READ runSummaries NOTIFY dashboardChanged)
    Q_PROPERTY(QStringList recentBackups READ recentBackups NOTIFY dashboardChanged)
    Q_PROPERTY(QStringList recentBackupSetIds READ recentBackupSetIds NOTIFY dashboardChanged)
    Q_PROPERTY(QStringList recentBackupTimestamps READ recentBackupTimestamps NOTIFY dashboardChanged)
    Q_PROPERTY(QString dashboardRefreshError READ dashboardRefreshError NOTIFY dashboardRefreshChanged)
    Q_PROPERTY(bool previewAvailable READ previewAvailable NOTIFY previewChanged)
    Q_PROPERTY(bool previewBusy READ previewBusy NOTIFY previewBusyChanged)
    Q_PROPERTY(QStringList previewIncluded READ previewIncluded NOTIFY previewChanged)
    Q_PROPERTY(QStringList previewExcluded READ previewExcluded NOTIFY previewChanged)
    Q_PROPERTY(QStringList previewSkipped READ previewSkipped NOTIFY previewChanged)
    Q_PROPERTY(QStringList previewMissing READ previewMissing NOTIFY previewChanged)
    Q_PROPERTY(QStringList cleanupTargets READ cleanupTargets NOTIFY cleanupChanged)
    Q_PROPERTY(bool cleanupConfirmationRequired READ cleanupConfirmationRequired NOTIFY cleanupChanged)

public:
    explicit BackupSetController(BackupEngine &engine, QString configPath, QObject *parent = nullptr);
    ~BackupSetController() override;

    QStringList setNames() const;
    QStringList setIds() const;
    QString currentId() const;
    int currentIndex() const;
    void setCurrentIndex(int index);
    QString currentName() const;
    void setCurrentName(const QString &name);
    QString currentRemoteRoot() const;
    void setCurrentRemoteRoot(const QString &remoteRoot);
    QStringList currentSources() const;
    void setCurrentSources(const QStringList &sources);
    QStringList currentExclusions() const;
    void setCurrentExclusions(const QStringList &exclusions);
    QString currentScheduleFrequency() const;
    void setCurrentScheduleFrequency(const QString &frequency);
    int currentScheduleHour() const;
    void setCurrentScheduleHour(int hour);
    int currentScheduleMinute() const;
    void setCurrentScheduleMinute(int minute);
    int currentScheduleWeekday() const;
    void setCurrentScheduleWeekday(int weekday);
    int currentScheduleDayOfMonth() const;
    void setCurrentScheduleDayOfMonth(int day);
    int currentRetention() const;
    void setCurrentRetention(int retention);
    bool currentOnlyOnAcPower() const;
    void setCurrentOnlyOnAcPower(bool enabled);
    QString currentNextRun() const;
    QString currentRunStatus() const;
    QString currentRunError() const;
    QStringList runningSetIds() const;
    QVariantMap remainingTimes() const;
    QVariantMap transferProgress() const;
    QVariantMap runDetails() const;
    QVariantMap runSummaries() const { return cachedRunSummaries; }
    QStringList recentBackups() const;
    QStringList recentBackupSetIds() const;
    QStringList recentBackupTimestamps() const;
    QStringList recentBackupCopyPaths() const;
    QString dashboardRefreshError() const { return refreshError; }
    bool previewAvailable() const;
    bool previewBusy() const { return previewWorking; }
    QStringList previewIncluded() const;
    QStringList previewExcluded() const;
    QStringList previewSkipped() const;
    QStringList previewMissing() const;
    QStringList cleanupTargets() const;
    bool cleanupConfirmationRequired() const;

    Q_INVOKABLE void addSet();
    Q_INVOKABLE void removeCurrentSet();
    Q_INVOKABLE void removeSet(int index);
    Q_INVOKABLE void preview();
    Q_INVOKABLE QString recentBackupFolderPath(const QString &setId) const;
    Q_INVOKABLE QVariantMap backupDetails(const QString &setId) const;
    Q_INVOKABLE void refreshRunState();
    Q_INVOKABLE bool save();
    Q_INVOKABLE bool exportSets(const QString &filePath);
    Q_INVOKABLE bool saveTemplate(const QString &filePath);
    Q_INVOKABLE bool importSets(const QString &filePath, bool merge = false);
    Q_INVOKABLE bool confirmCleanup();

signals:
    void setsChanged();
    void currentIndexChanged();
    void currentSetChanged();
    void previewChanged();
    void previewBusyChanged();
    void runStateChanged();
    void dashboardChanged();
    void runningSetIdsChanged();
    void remainingTimesChanged();
    void transferProgressChanged();
    void runDetailsChanged();
    void dashboardRefreshChanged();
    void cleanupChanged();
    void configurationSaved();
    void statusChanged(const QString &status);
    void failed(const QString &error);

private:
    BackupSet *currentSet();
    const BackupSet *currentSet() const;
    QVector<int> recentBackupIndexes() const;
    void clearPreview();
    void startPreview();
    void updateDashboard();
    bool isLocalStatePath(const QString &filePath) const;
    QStringList calculateRunningSetIds() const;
    QVariantMap calculateRemainingTimes() const;
    QVariantMap calculateTransferProgress() const;

    BackupEngine &engine;
    BackupConfigStore store;
    BackupRunStore runStore;
    CleanupStore cleanupStore;
    BackupConfig config;
    int selectedIndex = -1;
    BackupPreview previewResult;
    bool hasPreview = false;
    bool previewWorking = false;
    bool scanInFlight = false;
    quint64 previewGeneration = 0;
    quint64 activePreviewGeneration = 0;
    QStringList pendingSources;
    QStringList pendingExclusions;
    QFutureWatcher<BackupPreview> previewWatcher;
    std::shared_ptr<std::atomic_bool> previewCancelled;
    QTimer stateTimer;
    QString refreshError;
    QByteArray runContents;
    QByteArray cleanupContents;
    QStringList cachedRunningSetIds;
    QStringList cachedRecentBackups;
    QStringList cachedRecentSetIds;
    QStringList cachedRecentTimestamps;
    QVariantMap cachedRemainingTimes;
    QVariantMap cachedTransferProgress;
    QVariantMap cachedRunSummaries;
};

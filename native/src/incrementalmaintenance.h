#pragma once
#include <QObject>
#include <QFutureWatcher>
#include <QJsonObject>
#include <QVariantList>
#include <QStringList>
#include <atomic>
#include <memory>

// Owns its provider/runner on a background thread; never shares browser sessions.
class IncrementalMaintenance final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString integritySetId READ integritySetId NOTIFY busyChanged)
    Q_PROPERTY(QString activeSetId READ activeSetId NOTIFY busyChanged)
    Q_PROPERTY(QString activityText READ activityText NOTIFY busyChanged)
public:
    explicit IncrementalMaintenance(QString configPath, QObject *parent = nullptr);
    ~IncrementalMaintenance() override;
    bool busy() const { return working; }
    QString integritySetId() const { return checkingSet; }
    QString activeSetId() const { return working ? activeSet : QString(); }
    QString activityText() const { return working ? activity : QString(); }
    Q_INVOKABLE void previewCleanup(const QString &setId);
    Q_INVOKABLE void listVersions(const QString &setId);
    Q_INVOKABLE void previewDelete(const QString &setId, const QString &snapshotId = {});
    Q_INVOKABLE void previewDeleteVersions(const QString &setId, const QStringList &snapshotIds);
    Q_INVOKABLE void confirmCleanup();
    Q_INVOKABLE void cancelCleanup();
    Q_INVOKABLE void takeover(const QString &setId, bool oldWriterPermanentlyDisabled);
    Q_INVOKABLE void checkIntegrity(const QString &setId);
signals:
    void busyChanged();
    void cleanupReady(const QString &setId, const QString &proposal);
    void versionsReady(const QString &setId, const QVariantList &versions);
    void historyChanged(const QString &setId);
    void completed(const QString &message);
    void failed(const QString &message);
private:
    enum class Action { Preview, Delete, Versions, Confirm, Dismiss, Takeover, Integrity };
    void start(const QString &setId, Action action, const QString &confirmation = {}, bool oldWriterDisabled = false,
        const QStringList &snapshotIds = {});
    QString configPath;
    QFutureWatcher<QJsonObject> watcher;
    QString pendingSet;
    QString pendingConfirmation;
    QString checkingSet;
    QString activeSet;
    QString activity;
    bool working = false;
    std::shared_ptr<std::atomic_bool> stopped = std::make_shared<std::atomic_bool>(false);
};

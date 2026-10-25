#pragma once
#include <QObject>
#include <QFutureWatcher>
#include <QJsonObject>
#include <atomic>
#include <memory>

// Owns its provider/runner on a background thread; never shares browser sessions.
class IncrementalMaintenance final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString integritySetId READ integritySetId NOTIFY busyChanged)
public:
    explicit IncrementalMaintenance(QString configPath, QObject *parent = nullptr);
    ~IncrementalMaintenance() override;
    bool busy() const { return working; }
    QString integritySetId() const { return checkingSet; }
    Q_INVOKABLE void previewCleanup(const QString &setId);
    Q_INVOKABLE void confirmCleanup();
    Q_INVOKABLE void cancelCleanup();
    Q_INVOKABLE void takeover(const QString &setId, bool oldWriterPermanentlyDisabled);
    Q_INVOKABLE void checkIntegrity(const QString &setId);
signals:
    void busyChanged();
    void cleanupReady(const QString &setId, const QString &proposal);
    void completed(const QString &message);
    void failed(const QString &message);
private:
    enum class Action { Preview, Confirm, Dismiss, Takeover, Integrity };
    void start(const QString &setId, Action action, const QString &confirmation = {}, bool oldWriterDisabled = false);
    QString configPath;
    QFutureWatcher<QJsonObject> watcher;
    QString pendingSet;
    QString pendingConfirmation;
    QString checkingSet;
    bool working = false;
    std::shared_ptr<std::atomic_bool> stopped = std::make_shared<std::atomic_bool>(false);
};

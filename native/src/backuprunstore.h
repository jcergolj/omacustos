#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>
#include <functional>
#include "backupconfig.h"
#include "backupprogress.h"
#include "backupresult.h"

struct BackupRunRecord {
    QString setId;
    QString status = QStringLiteral("idle");
    QString reason;
    QString lastError;
    int attempts = 0;
    QDateTime scheduledFor;
    QDateTime nextAttempt;
    QDateTime lastScheduled;
    QDateTime nextScheduled;
    QDateTime lastSuccess;
    QDateTime lastFailure;
    QString remoteCopyPath;
    BackupProgress progress;
    qint64 progressElapsedMs = 0;
    QDateTime progressUpdatedAt;
    qint64 lastSuccessfulElapsedMs = 0;
    qint64 lastSuccessfulBytes = 0;
    int lastSuccessfulFiles = 0;
    BackupResult result;

    // -1 means insufficient progress; zero means the estimate has elapsed.
    qint64 estimatedRemainingSeconds(const QDateTime &now) const;
};

class BackupRunStore final
{
public:
    explicit BackupRunStore(QString path);

    bool load(QString *error = nullptr, QByteArray *contents = nullptr);
    bool loadFromBytes(const QByteArray &contents, QString *error = nullptr);
    bool save(QString *error = nullptr) const;
    QString filePath() const;
    QVector<BackupRunRecord> &records();
    const QVector<BackupRunRecord> &records() const;

    // Durable mutations reload under a short queue lock. Worker exclusion is
    // separate and must be held by the executable for prepare/attempt operations.
    bool queueManual(const QStringList &setIds, bool requireNew, QString *error = nullptr);
    bool prepareWorker(const QVector<BackupSet> &sets, const QDateTime &now, QString *error = nullptr);
    bool waitForPrerequisite(const QString &setId, const QString &reason, const QDateTime &now,
        QString *error = nullptr);
    bool beginAttempt(const QString &setId, const QString &copyPath, BackupRunRecord *attempt,
        QString *error = nullptr);
    bool publishProgress(const BackupRunRecord &attempt, QString *error = nullptr);
    bool completeAttempt(const BackupRunRecord &attempt, bool succeeded, const QString &failure,
        const QDateTime &now, QString *error = nullptr);
    bool rememberCopyPath(const BackupRunRecord &expected, const QString &copyPath, QString *error = nullptr);
    bool markCopyDeleted(const QString &setId, const QString &copyPath, QString *error = nullptr);

    void ensureSet(const QString &setId);
    bool enqueue(const QString &setId, const QString &reason, const QDateTime &scheduledFor);
    QVector<int> readyIndexes(const QDateTime &now) const;
    // In-memory rules; durable callers use prepareWorker instead.
    bool recoverInterrupted(const QDateTime &now);
    BackupRunRecord *find(const QString &setId);
    const BackupRunRecord *find(const QString &setId) const;
    void markRunning(BackupRunRecord &record);
    void markSuccess(BackupRunRecord &record, const QDateTime &now);
    void markWaiting(BackupRunRecord &record, const QString &reason, const QDateTime &now);
    void markRetrying(BackupRunRecord &record, const QString &error, const QDateTime &now);
    void markIncomplete(BackupRunRecord &record, const QString &error, const QDateTime &now);
    void markFailed(BackupRunRecord &record, const QString &error, const QDateTime &now);
    void markAuthenticationRequired(BackupRunRecord &record, const QString &error, const QDateTime &now);

    static int retryDelaySeconds(int attempt);

private:
    bool update(const std::function<bool(BackupRunStore &, QString *)> &mutation, QString *error);
    BackupRunRecord *matchingAttempt(const BackupRunRecord &attempt, QString *error);
    QString path;
    QVector<BackupRunRecord> runRecords;
};

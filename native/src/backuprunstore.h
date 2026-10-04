#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>
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

    void ensureSet(const QString &setId);
    bool enqueue(const QString &setId, const QString &reason, const QDateTime &scheduledFor);
    QVector<int> readyIndexes(const QDateTime &now) const;
    // Only call while holding the exclusive worker and run-state locks.
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
    QString path;
    QVector<BackupRunRecord> runRecords;
};

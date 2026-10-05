#include "backuprunstore.h"
#include "backupschedule.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <cmath>
#include <limits>

namespace {

QDateTime readDate(const QJsonObject &object, const QString &key)
{
    return QDateTime::fromString(object.value(key).toString(), Qt::ISODateWithMs);
}

void writeDate(QJsonObject &object, const QString &key, const QDateTime &value)
{
    if (value.isValid()) {
        object.insert(key, value.toString(Qt::ISODateWithMs));
    }
}

}

qint64 BackupRunRecord::estimatedRemainingSeconds(const QDateTime &now) const
{
    if (status != QStringLiteral("running") || progress.finalizing
        || progress.totalFiles <= 0 || progress.processedFiles < 0
        || progress.processedFiles >= progress.totalFiles
        || progressElapsedMs < 0 || !progressUpdatedAt.isValid() || !now.isValid()
        || now < progressUpdatedAt || progress.processedBytes < 0
        || progress.totalBytes < progress.processedBytes) {
        return -1;
    }
    double estimateMs;
    if (progress.processedFiles > 0) {
        if (progressElapsedMs == 0 || (progress.totalBytes > 0 && progress.processedBytes == 0)) {
            return -1;
        }
        double remainingRatio = double(progress.totalFiles - progress.processedFiles) / progress.processedFiles;
        if (progress.processedBytes > 0) {
            remainingRatio = qMax(remainingRatio,
                double(progress.totalBytes - progress.processedBytes) / progress.processedBytes);
        }
        estimateMs = remainingRatio * progressElapsedMs;
    } else {
        // Previous successful runs can provide an initial estimate, including
        // single-file backups that cannot report progress until upload finishes.
        if (lastSuccessfulElapsedMs <= 0 || lastSuccessfulFiles <= 0
            || (progress.totalBytes > 0 && lastSuccessfulBytes <= 0)) {
            return -1;
        }
        double ratio = double(progress.totalFiles) / lastSuccessfulFiles;
        if (lastSuccessfulBytes > 0) {
            ratio = qMax(ratio, double(progress.totalBytes) / lastSuccessfulBytes);
        }
        estimateMs = ratio * lastSuccessfulElapsedMs - progressElapsedMs;
    }
    // Account for both per-file overhead and byte throughput, using the slower
    // estimate. Count down from the last monotonic elapsed-time sample.
    const double remainingMs = estimateMs - progressUpdatedAt.msecsTo(now);
    if (!std::isfinite(remainingMs) || remainingMs / 1000.0 >= double(std::numeric_limits<qint64>::max())) {
        return -1;
    }
    return remainingMs <= 0 ? 0 : qint64(std::ceil(remainingMs / 1000.0));
}

BackupRunStore::BackupRunStore(QString path)
    : path(std::move(path))
{
}

bool BackupRunStore::load(QString *error, QByteArray *contents)
{
    QFile file(path);
    if (!file.exists()) {
        runRecords.clear();
        if (contents) *contents = QByteArray(1, '\0');
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("The backup run state could not be opened.");
        }
        return false;
    }

    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("The backup run state could not be read.");
        }
        return false;
    }
    if (!loadFromBytes(bytes, error)) {
        return false;
    }
    if (contents) *contents = QByteArray(1, '\1') + bytes;
    return true;
}

bool BackupRunStore::loadFromBytes(const QByteArray &contents, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(contents, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("The backup run state is malformed.");
        }
        return false;
    }

    const QJsonValue runs = document.object().value(QStringLiteral("runs"));
    if (!runs.isUndefined() && !runs.isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("The backup run state is malformed.");
        }
        return false;
    }
    QVector<BackupRunRecord> nextRecords;
    const QJsonArray records = runs.toArray();
    for (const QJsonValue &value : records) {
        if (!value.isObject()) {
            if (error != nullptr) {
                *error = QStringLiteral("The backup run state contains an invalid record.");
            }
            return false;
        }
        const QJsonObject object = value.toObject();
        BackupRunRecord record;
        record.setId = object.value(QStringLiteral("set_id")).toString();
        record.status = object.value(QStringLiteral("status")).toString(QStringLiteral("idle"));
        record.reason = object.value(QStringLiteral("reason")).toString();
        record.lastError = object.value(QStringLiteral("last_error")).toString();
        record.attempts = object.value(QStringLiteral("attempts")).toInt();
        record.scheduledFor = readDate(object, QStringLiteral("scheduled_for"));
        record.nextAttempt = readDate(object, QStringLiteral("next_attempt"));
        record.lastScheduled = readDate(object, QStringLiteral("last_scheduled"));
        record.nextScheduled = readDate(object, QStringLiteral("next_scheduled"));
        record.lastSuccess = readDate(object, QStringLiteral("last_success"));
        record.lastFailure = readDate(object, QStringLiteral("last_failure"));
        record.remoteCopyPath = object.value(QStringLiteral("remote_copy_path")).toString();
        const QJsonObject progress = object.value(QStringLiteral("progress")).toObject();
        record.progress.totalBytes = qMax(qint64(0), progress.value(QStringLiteral("total_bytes")).toInteger());
        record.progress.processedBytes = qMax(qint64(0), progress.value(QStringLiteral("processed_bytes")).toInteger());
        record.progress.totalFiles = qMax(0, progress.value(QStringLiteral("total_files")).toInt());
        record.progress.processedFiles = qMax(0, progress.value(QStringLiteral("processed_files")).toInt());
        record.progress.finalizing = progress.value(QStringLiteral("finalizing")).toBool();
        record.progress.verifiedFiles = qMax(0, progress.value(QStringLiteral("verified_files")).toInt());
        record.progress.verifiedBytes = qMax(qint64(0), progress.value(QStringLiteral("verified_bytes")).toInteger());
        record.progress.failedItems = qMax(0, progress.value(QStringLiteral("failed_items")).toInt());
        record.progress.currentFile = progress.value(QStringLiteral("current_file")).toString();
        record.progress.currentFileBytes = qMax(qint64(0), progress.value(QStringLiteral("current_file_bytes")).toInteger());
        record.progress.phase = progress.value(QStringLiteral("phase")).toString();
        record.progressElapsedMs = qMax(qint64(0), progress.value(QStringLiteral("elapsed_ms")).toInteger());
        record.progressUpdatedAt = readDate(progress, QStringLiteral("updated_at"));
        record.lastSuccessfulElapsedMs = qMax(qint64(0), object.value(QStringLiteral("last_successful_elapsed_ms")).toInteger());
        record.lastSuccessfulBytes = qMax(qint64(0), object.value(QStringLiteral("last_successful_bytes")).toInteger());
        record.lastSuccessfulFiles = qMax(0, object.value(QStringLiteral("last_successful_files")).toInt());
        const QJsonObject result = object.value(QStringLiteral("result")).toObject();
        record.result.reported = result.value(QStringLiteral("reported")).toBool();
        record.result.manifestVerified = result.value(QStringLiteral("manifest_verified")).toBool();
        record.result.verifiedFiles = qMax(0, result.value(QStringLiteral("verified_files")).toInt());
        record.result.verifiedBytes = qMax(qint64(0), result.value(QStringLiteral("verified_bytes")).toInteger());
        for (const QJsonValue &value : result.value(QStringLiteral("issues")).toArray()) {
            const QJsonObject issue = value.toObject();
            record.result.issues.append({issue.value(QStringLiteral("path")).toString(),
                issue.value(QStringLiteral("phase")).toString(), issue.value(QStringLiteral("reason")).toString()});
        }
        if (record.setId.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("The backup run state contains an invalid record.");
            }
            return false;
        }
        nextRecords.append(record);
    }

    runRecords = std::move(nextRecords);
    return true;
}

bool BackupRunStore::save(QString *error) const
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to create the backup run state directory.");
        }
        return false;
    }

    QJsonArray records;
    for (const BackupRunRecord &record : runRecords) {
        QJsonObject object {
            {QStringLiteral("set_id"), record.setId},
            {QStringLiteral("status"), record.status},
            {QStringLiteral("reason"), record.reason},
            {QStringLiteral("last_error"), record.lastError},
            {QStringLiteral("attempts"), record.attempts},
            {QStringLiteral("remote_copy_path"), record.remoteCopyPath},
            {QStringLiteral("last_successful_elapsed_ms"), record.lastSuccessfulElapsedMs},
            {QStringLiteral("last_successful_bytes"), record.lastSuccessfulBytes},
            {QStringLiteral("last_successful_files"), record.lastSuccessfulFiles},
        };
        writeDate(object, QStringLiteral("scheduled_for"), record.scheduledFor);
        writeDate(object, QStringLiteral("next_attempt"), record.nextAttempt);
        writeDate(object, QStringLiteral("last_scheduled"), record.lastScheduled);
        writeDate(object, QStringLiteral("next_scheduled"), record.nextScheduled);
        writeDate(object, QStringLiteral("last_success"), record.lastSuccess);
        writeDate(object, QStringLiteral("last_failure"), record.lastFailure);
        QJsonObject progress {
            {QStringLiteral("total_bytes"), record.progress.totalBytes},
            {QStringLiteral("processed_bytes"), record.progress.processedBytes},
            {QStringLiteral("total_files"), record.progress.totalFiles},
            {QStringLiteral("processed_files"), record.progress.processedFiles},
            {QStringLiteral("finalizing"), record.progress.finalizing},
            {QStringLiteral("verified_files"), record.progress.verifiedFiles},
            {QStringLiteral("verified_bytes"), record.progress.verifiedBytes},
            {QStringLiteral("failed_items"), record.progress.failedItems},
            {QStringLiteral("current_file"), record.progress.currentFile},
            {QStringLiteral("current_file_bytes"), record.progress.currentFileBytes},
            {QStringLiteral("phase"), record.progress.phase},
            {QStringLiteral("elapsed_ms"), record.progressElapsedMs},
        };
        writeDate(progress, QStringLiteral("updated_at"), record.progressUpdatedAt);
        object.insert(QStringLiteral("progress"), progress);
        QJsonArray issues;
        for (const BackupIssue &issue : record.result.issues) {
            issues.append(QJsonObject {{QStringLiteral("path"), issue.path},
                {QStringLiteral("phase"), issue.phase}, {QStringLiteral("reason"), issue.reason}});
        }
        object.insert(QStringLiteral("result"), QJsonObject {
            {QStringLiteral("reported"), record.result.reported},
            {QStringLiteral("manifest_verified"), record.result.manifestVerified},
            {QStringLiteral("verified_files"), record.result.verifiedFiles},
            {QStringLiteral("verified_bytes"), record.result.verifiedBytes},
            {QStringLiteral("issues"), issues},
        });
        records.append(object);
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to write the backup run state.");
        }
        return false;
    }
    const QByteArray contents = QJsonDocument(QJsonObject {{QStringLiteral("runs"), records}}).toJson(QJsonDocument::Indented);
    if (file.write(contents) != contents.size() || !file.commit()) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to finish writing the backup run state.");
        }
        return false;
    }
    return true;
}

bool BackupRunStore::update(const std::function<bool(BackupRunStore &, QString *)> &mutation, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) *error = QStringLiteral("Unable to create the backup run state directory.");
        return false;
    }
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(5000)) {
        if (error) *error = QStringLiteral("A backup queue update is already in progress.");
        return false;
    }
    BackupRunStore fresh(path);
    if (!fresh.load(error) || !mutation(fresh, error) || !fresh.save(error)) {
        return false;
    }
    runRecords = std::move(fresh.runRecords);
    return true;
}

bool BackupRunStore::queueManual(const QStringList &setIds, bool requireNew, QString *error)
{
    return update([&](BackupRunStore &fresh, QString *error) {
        const QDateTime now = QDateTime::currentDateTime();
        bool queued = false;
        for (const QString &setId : setIds) {
            queued = fresh.enqueue(setId, QStringLiteral("manual"), now) || queued;
        }
        if (requireNew && !queued) {
            if (error) *error = QStringLiteral("The selected backup is already queued.");
            return false;
        }
        return true;
    }, error);
}

bool BackupRunStore::prepareWorker(const QVector<BackupSet> &sets, const QDateTime &now, QString *error)
{
    return update([&](BackupRunStore &fresh, QString *) {
        fresh.recoverInterrupted(now);
        for (const BackupSet &set : sets) {
            fresh.ensureSet(set.id);
            BackupRunRecord *record = fresh.find(set.id);
            if (!set.schedule.enabled()) continue;
            QDateTime due = record->nextScheduled;
            if (record->lastScheduled.isValid()) {
                const QDateTime recalculated = BackupScheduleCalculator::nextRun(set.schedule, record->lastScheduled);
                if (recalculated.isValid() && recalculated != due) {
                    due = recalculated;
                    record->nextScheduled = recalculated;
                }
            }
            if (!due.isValid()) {
                const QDateTime currentDue = BackupScheduleCalculator::dueRun(set.schedule, now);
                if (currentDue.isValid() && currentDue <= now) {
                    due = currentDue;
                } else {
                    record->nextScheduled = BackupScheduleCalculator::nextRun(set.schedule, now);
                }
            }
            if (due.isValid() && due <= now) {
                record->lastScheduled = due;
                record->nextScheduled = BackupScheduleCalculator::nextRun(set.schedule, now);
                fresh.enqueue(set.id, QStringLiteral("schedule"), due);
            }
        }
        return true;
    }, error);
}

bool BackupRunStore::waitForPrerequisite(const QString &setId, const QString &reason,
    const QDateTime &now, QString *error)
{
    return update([&](BackupRunStore &fresh, QString *error) {
        BackupRunRecord *record = fresh.find(setId);
        if (!record) {
            if (error) *error = QStringLiteral("The queued backup no longer exists.");
            return false;
        }
        fresh.markWaiting(*record, reason, now);
        return true;
    }, error);
}

bool BackupRunStore::beginAttempt(const QString &setId, const QString &copyPath,
    BackupRunRecord *attempt, QString *error)
{
    BackupRunRecord claimed;
    const bool saved = update([&](BackupRunStore &fresh, QString *error) {
        BackupRunRecord *record = fresh.find(setId);
        const QVector<int> ready = fresh.readyIndexes(QDateTime::currentDateTime());
        bool eligible = false;
        for (int index : ready) eligible = eligible || fresh.records().at(index).setId == setId;
        if (!record || !eligible) {
            if (error) *error = QStringLiteral("The backup is no longer ready to run.");
            return false;
        }
        fresh.markRunning(*record);
        record->remoteCopyPath = copyPath;
        claimed = *record;
        return true;
    }, error);
    if (saved) *attempt = std::move(claimed);
    return saved;
}

BackupRunRecord *BackupRunStore::matchingAttempt(const BackupRunRecord &attempt, QString *error)
{
    BackupRunRecord *record = find(attempt.setId);
    if (!record || record->status != QStringLiteral("running")
        || record->remoteCopyPath != attempt.remoteCopyPath || record->attempts != attempt.attempts
        || record->scheduledFor != attempt.scheduledFor) {
        if (error) *error = QStringLiteral("The backup attempt is no longer current.");
        return nullptr;
    }
    return record;
}

bool BackupRunStore::publishProgress(const BackupRunRecord &attempt, QString *error)
{
    return update([&](BackupRunStore &fresh, QString *error) {
        BackupRunRecord *record = fresh.matchingAttempt(attempt, error);
        if (!record) return false;
        record->progress = attempt.progress;
        record->progressElapsedMs = attempt.progressElapsedMs;
        record->progressUpdatedAt = attempt.progressUpdatedAt;
        return true;
    }, error);
}

bool BackupRunStore::completeAttempt(const BackupRunRecord &attempt, bool succeeded,
    const QString &failure, const QDateTime &now, QString *error)
{
    return update([&](BackupRunStore &fresh, QString *error) {
        BackupRunRecord *record = fresh.matchingAttempt(attempt, error);
        if (!record) return false;
        record->progress = attempt.progress;
        record->progressElapsedMs = attempt.progressElapsedMs;
        record->progressUpdatedAt = attempt.progressUpdatedAt;
        record->result = attempt.result;
        const QString message = failure.toLower();
        if (succeeded) {
            fresh.markSuccess(*record, now);
        } else if (message.contains(QStringLiteral("auth")) || message.contains(QStringLiteral("sign in"))
            || message.contains(QStringLiteral("login")) || message.contains(QStringLiteral("401"))) {
            fresh.markAuthenticationRequired(*record, failure, now);
        } else if (attempt.result.manifestVerified && attempt.result.verifiedFiles > 0) {
            fresh.markIncomplete(*record, failure, now);
        } else {
            fresh.markFailed(*record, failure, now);
        }
        return true;
    }, error);
}

QString BackupRunStore::filePath() const
{
    return path;
}

bool BackupRunStore::rememberCopyPath(const BackupRunRecord &expected, const QString &copyPath, QString *error)
{
    return update([&](BackupRunStore &fresh, QString *error) {
        fresh.ensureSet(expected.setId);
        BackupRunRecord *record = fresh.find(expected.setId);
        if (record->status != expected.status || record->scheduledFor != expected.scheduledFor
            || record->attempts != expected.attempts || record->remoteCopyPath != expected.remoteCopyPath
            || record->lastSuccess != expected.lastSuccess) {
            if (error) *error = QStringLiteral("The recent backup changed. Open it again to use the current copy.");
            return false;
        }
        record->remoteCopyPath = copyPath;
        return true;
    }, error);
}

bool BackupRunStore::markCopyDeleted(const QString &setId, const QString &copyPath, QString *error)
{
    return update([&](BackupRunStore &fresh, QString *error) {
        BackupRunRecord *record = fresh.find(setId);
        if (!record || record->remoteCopyPath != copyPath || record->status == QStringLiteral("running")) {
            if (error) *error = QStringLiteral("The recent backup copy changed. Refresh it to use the current copy.");
            return false;
        }
        record->remoteCopyPath.clear();
        // A manual request may arrive while the old copy is being trashed.
        // Removing that pointer must not cancel the newly queued logical run.
        if (record->status != QStringLiteral("pending")) {
            record->status = QStringLiteral("copy_deleted");
            record->nextAttempt = {};
            record->lastError.clear();
        }
        return true;
    }, error);
}

QVector<BackupRunRecord> &BackupRunStore::records()
{
    return runRecords;
}

const QVector<BackupRunRecord> &BackupRunStore::records() const
{
    return runRecords;
}

void BackupRunStore::ensureSet(const QString &setId)
{
    if (find(setId) == nullptr) {
        runRecords.append({setId});
    }
}

bool BackupRunStore::enqueue(const QString &setId, const QString &reason, const QDateTime &scheduledFor)
{
    ensureSet(setId);
    BackupRunRecord *record = find(setId);
    if (record->status == QStringLiteral("pending") || record->status == QStringLiteral("running")
        || record->status == QStringLiteral("retrying") || record->status == QStringLiteral("waiting")) {
        return false;
    }

    record->status = QStringLiteral("pending");
    record->progress = {};
    record->result = {};
    record->progressElapsedMs = 0;
    record->progressUpdatedAt = {};
    record->reason = reason;
    record->lastError.clear();
    record->scheduledFor = scheduledFor;
    record->nextAttempt = scheduledFor.isValid() ? scheduledFor : QDateTime::currentDateTime();
    return true;
}

QVector<int> BackupRunStore::readyIndexes(const QDateTime &now) const
{
    QVector<int> indexes;
    for (int index = 0; index < runRecords.size(); ++index) {
        const BackupRunRecord &record = runRecords.at(index);
        if ((record.status == QStringLiteral("pending") || record.status == QStringLiteral("retrying")
              || record.status == QStringLiteral("waiting") || record.status == QStringLiteral("incomplete")
              || record.status == QStringLiteral("failed"))
            && (!record.nextAttempt.isValid() || record.nextAttempt <= now)) {
            indexes.append(index);
        }
    }
    return indexes;
}

bool BackupRunStore::recoverInterrupted(const QDateTime &now)
{
    bool recovered = false;
    for (BackupRunRecord &record : runRecords) {
        if (record.status != QStringLiteral("running")) {
            continue;
        }
        markRetrying(record, QStringLiteral("The previous backup was interrupted; retrying."), now);
        record.nextAttempt = now;
        record.progress = {};
        record.progressElapsedMs = 0;
        record.progressUpdatedAt = {};
        recovered = true;
    }
    return recovered;
}

BackupRunRecord *BackupRunStore::find(const QString &setId)
{
    for (BackupRunRecord &record : runRecords) {
        if (record.setId == setId) {
            return &record;
        }
    }
    return nullptr;
}

const BackupRunRecord *BackupRunStore::find(const QString &setId) const
{
    for (const BackupRunRecord &record : runRecords) {
        if (record.setId == setId) {
            return &record;
        }
    }
    return nullptr;
}

void BackupRunStore::markRunning(BackupRunRecord &record)
{
    record.status = QStringLiteral("running");
    record.progress = {};
    record.result = {};
    record.progressElapsedMs = 0;
    record.progressUpdatedAt = {};
    record.attempts++;
}

void BackupRunStore::markSuccess(BackupRunRecord &record, const QDateTime &now)
{
    if (record.progressElapsedMs > 0 && record.progress.totalFiles > 0
        && record.progress.processedFiles == record.progress.totalFiles) {
        record.lastSuccessfulElapsedMs = record.progressElapsedMs;
        record.lastSuccessfulBytes = record.progress.totalBytes;
        record.lastSuccessfulFiles = record.progress.totalFiles;
    }
    record.status = QStringLiteral("success");
    record.lastSuccess = now;
    record.lastError.clear();
    record.nextAttempt = {};
    record.attempts = 0;
}

void BackupRunStore::markWaiting(BackupRunRecord &record, const QString &reason, const QDateTime &now)
{
    record.status = QStringLiteral("waiting");
    record.lastError = reason;
    record.nextAttempt = now.addSecs(60);
}

void BackupRunStore::markRetrying(BackupRunRecord &record, const QString &error, const QDateTime &now)
{
    record.status = QStringLiteral("retrying");
    record.lastError = error;
    record.lastFailure = now;
    record.nextAttempt = now.addSecs(retryDelaySeconds(record.attempts));
}

void BackupRunStore::markIncomplete(BackupRunRecord &record, const QString &error, const QDateTime &now)
{
    record.status = QStringLiteral("incomplete");
    record.lastError = error;
    record.lastFailure = now;
    record.nextAttempt = now.addSecs(retryDelaySeconds(record.attempts));
}

void BackupRunStore::markFailed(BackupRunRecord &record, const QString &error, const QDateTime &now)
{
    record.status = QStringLiteral("failed");
    record.lastError = error;
    record.lastFailure = now;
    record.nextAttempt = now.addSecs(retryDelaySeconds(record.attempts));
}

void BackupRunStore::markAuthenticationRequired(BackupRunRecord &record, const QString &error, const QDateTime &now)
{
    record.status = QStringLiteral("authentication_required");
    record.lastError = error;
    record.lastFailure = now;
    record.nextAttempt = {};
}

int BackupRunStore::retryDelaySeconds(int attempt)
{
    return qMin(3600, 5 * (1 << qMin(9, qMax(0, attempt - 1))));
}

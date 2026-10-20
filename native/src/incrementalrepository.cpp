#include "incrementalrepository.h"
#include "incrementalmanifest.h"
#include "backuparchive.h"
#include "backupstaging.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMap>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <fcntl.h>
#include <unistd.h>

namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QString digest(const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }

bool syncDirectory(const QString &path)
{
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    const bool success = fd >= 0 && ::fsync(fd) == 0;
    if (fd >= 0) ::close(fd);
    return success;
}

bool save(const QString &path, const QJsonObject &object, QString *error)
{
    const QString parent = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(parent) || QFileInfo(path).isSymLink()) {
        *error = "Waiting for journal storage: unsafe or unavailable checkpoint directory.";
        return false;
    }
    // Persist newly created parent links as well as the final atomic rename.
    for (QString directory = parent; directory != "/"; directory = QFileInfo(directory).absolutePath()) {
        if (QFileInfo(directory).isSymLink() || !syncDirectory(directory)) {
            *error = "Waiting for journal storage: unable to sync checkpoint parents.";
            return false;
        }
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const QByteArray bytes = json(object);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit() || !syncDirectory(parent)) {
        *error = "Waiting for journal storage: unable to durably checkpoint: " + file.errorString();
        return false;
    }
    return true;
}

QJsonObject anchor(const IncrementalWriteRequest &request)
{
    return {{"version", 1}, {"application", "omacustos"}, {"coordination", "manual-offline-v1"},
        {"set_id", request.scan.setId}, {"origin_computer_id", request.originComputerId},
        {"owner_computer_id", request.computerId}, {"ownership_epoch", request.ownershipEpoch}};
}

QStringList sourcePaths(const IncrementalWriteRequest &request)
{
    QStringList paths;
    for (const auto &source : request.scan.sources) paths.append(source.sourcePath);
    return paths;
}

bool validRequest(const IncrementalWriteRequest &request, QString *error)
{
    if (!IncrementalManifest::validId(request.scan.setId) || !IncrementalManifest::validId(request.computerId)
        || !IncrementalManifest::validId(request.originComputerId) || !IncrementalManifest::validId(request.operationId)
        || !IncrementalManifest::validId(request.snapshotId) || request.ownershipEpoch < 1
        || request.ownershipEpoch > 9007199254740991LL || !request.scan.checkedAt.isValid()
        || request.remoteSetRoot != QDir::cleanPath(request.remoteSetRoot)
        || request.remoteSetRoot.split('/').contains("..") || request.remoteSetRoot.contains('\\')
        || request.remoteSetRoot.contains(QChar::Null)
        || !request.remoteSetRoot.endsWith("sets/" + request.scan.setId)
        || (request.remoteSetRoot != "sets/" + request.scan.setId
            && !request.remoteSetRoot.endsWith("/sets/" + request.scan.setId))
        || request.stagingBudget <= 0 || request.stagingBudget > 9007199254740991LL
        || request.archiveTargetBytes <= 0 || request.archiveTargetBytes > 9007199254740991LL || request.batchFileLimit <= 0) {
        *error = "Invalid incremental repository identity, path, time or staging limits.";
        return false;
    }
    return true;
}

// All remote writes are append-only except the separately managed ownership
// anchor. It is re-read before every mutation, not mistaken for server fencing.
class Store {
public:
    Store(const IncrementalWriteRequest &request, BackupProvider &provider)
        : request(request), provider(provider) {}
    QString error;
    BackupStaging staging;
    QString journal;
    std::unique_ptr<QLockFile> lock;
    bool verificationSpace(const QJsonObject &archive)
    {
        if (archive.value("size").toInteger() > request.stagingBudget
            && archive.value("members").toArray().size() != 1) {
            error = "Waiting for verification staging storage: this multi-file archive exceeds the current byte budget.";
            return false;
        }
        return staging.hasSpace(archive.value("size").toInteger(), &error);
    }

    bool open()
    {
        if (!validRequest(request, &error)) return false;
        QString state = request.stateDirectory;
        if (state.isEmpty()) state = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath("omacustos/incremental");
        if (!BackupStaging::validate(state, sourcePaths(request), &error) || !QDir().mkpath(state)) return false;
        state = QFileInfo(state).canonicalFilePath();
        const QString key = digest(request.remoteSetRoot.toUtf8());
        lock = std::make_unique<QLockFile>(QDir(state).filePath(request.scan.setId + ".lock"));
        lock->setStaleLockTime(0);
        if (!lock->tryLock(0)) { error = "Another writer or cleanup operation holds this set's local mutation lock."; return false; }
        const QString setJournal = QDir(state).filePath(key);
        journal = QDir(setJournal).filePath(request.operationId);
        if (QFileInfo(setJournal).isSymLink() || QFileInfo(journal).isSymLink() || !QDir().mkpath(journal)
            || !QFile::setPermissions(journal, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
            error = "Waiting for private journal storage.";
            return false;
        }
        return staging.open(request.stagingDirectory, sourcePaths(request), &error);
    }
    QString remote(const QString &relative) const { return relative.isEmpty() ? request.remoteSetRoot : request.remoteSetRoot + '/' + relative; }
    RemoteLookup lookup(const QString &relative) { return provider.lookup(remote(relative), &error); }
    bool get(const QString &relative, QJsonObject *object)
    {
        QTemporaryDir directory(QDir(staging.path()).filePath("metadata-XXXXXX"));
        if (!directory.isValid()) { error = "Waiting for metadata staging storage."; return false; }
        const QString path = directory.filePath(QFileInfo(relative).fileName());
        if (!provider.download(remote(relative), path, &error)) return false;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return false; }
        const auto document = QJsonDocument::fromJson(file.readAll());
        if (file.error() != QFileDevice::NoError || !document.isObject()) {
            error = "Unreadable or malformed remote control record: " + relative;
            return false;
        }
        *object = document.object();
        return true;
    }
    bool owner()
    {
        QJsonObject current;
        if (!get("repository.json", &current)) return false;
        if (current != anchor(request)) { error = "Repository owner/epoch/identity changed; writes are forbidden. Offline handoff is required."; return false; }
        return true;
    }
    bool local(const QString &relative, const QJsonObject &object) { return save(QDir(journal).filePath(relative), object, &error); }
    bool put(const QString &relative, const QJsonObject &object, bool initializing = false)
    {
        if (!initializing && !owner()) return false;
        if (!provider.ensureDirectory(QFileInfo(remote(relative)).path(), &error)) return false;
        const auto found = lookup(relative);
        if (found == RemoteLookup::Unknown) return false;
        if (found == RemoteLookup::Missing) {
            QTemporaryDir directory(QDir(staging.path()).filePath("control-XXXXXX"));
            if (!directory.isValid()) { error = "Waiting for control staging storage."; return false; }
            const QString path = directory.filePath(QFileInfo(relative).fileName());
            if (!save(path, object, &error) || !provider.uploadImmutable(path, remote(relative), &error)) return false;
        }
        QJsonObject observed;
        if (!get(relative, &observed)) return false;
        if (observed != object) { error = "Immutable remote record conflicts with the prepared identity: " + relative; return false; }
        return true;
    }
    bool children(const QString &relative, QStringList *names)
    {
        QVector<RemoteItem> items;
        if (!provider.list(remote(relative), &items, &error)) return false;
        QSet<QString> seen;
        for (const auto &item : items) {
            if (item.name.isEmpty() || item.name.contains('/') || item.name.contains(QChar::Null)
                || item.name == "." || item.name == ".." || seen.contains(item.name)
                || item.path != remote(relative) + '/' + item.name) {
                error = "Ambiguous or unsafe repository listing: " + relative;
                return false;
            }
            seen.insert(item.name);
        }
        *names = seen.values();
        names->sort();
        return true;
    }
private:
    const IncrementalWriteRequest &request;
    BackupProvider &provider;
};

QVector<BackupEntry> inventory(const QJsonObject &archive)
{
    BackupArchive physical;
    for (const auto &value : archive.value("members").toArray()) physical.members.append(value.toObject().value("path").toString());
    QVector<BackupEntry> entries;
    for (const auto &value : archive.value("members").toArray()) {
        const auto member = value.toObject();
        BackupEntry entry;
        entry.size = member.value("size").toInteger();
        entry.checksum = QByteArray::fromHex(member.value("sha256").toString().toLatin1());
        entry.memberPath = member.value("path").toString();
        entry.archive = physical;
        entries.append(entry);
    }
    return entries;
}

QJsonObject manifestBase(const IncrementalWriteRequest &request, const QDateTime &createdAt)
{
    return {{"version", 4}, {"application", "omacustos"}, {"set_id", request.scan.setId},
        {"origin_computer_id", request.originComputerId}, {"owner_computer_id", request.computerId},
        {"ownership_epoch", request.ownershipEpoch}, {"snapshot_id", request.snapshotId},
        {"created_at", createdAt.toUTC().toString(Qt::ISODateWithMs)}, {"status", "complete"}};
}

// Reuse the authoritative schema boundary for prepared/verified descriptors.
// A synthetic receipt validates structure ONLY; prepared state never certifies bytes.
bool validArchive(const QJsonObject &archive, bool verified, const IncrementalWriteRequest &request, QString *error)
{
    QJsonObject descriptor = archive;
    if (!verified) {
        if (archive.contains("original_verification")) { *error = "Prepared object incorrectly claims verified evidence."; return false; }
        descriptor["original_verification"] = QJsonObject{{"method", "download-sha256"},
            {"verified_at", request.scan.checkedAt.toUTC().toString(Qt::ISODateWithMs)}, {"sha256", archive.value("sha256")}};
    }
    auto document = manifestBase(request, request.scan.checkedAt);
    QJsonArray entries;
    for (const auto &value : descriptor.value("members").toArray()) {
        const auto member = value.toObject();
        entries.append(QJsonObject{{"restore", member.value("path")}, {"archive", descriptor.value("id")},
            {"member", member.value("path")}, {"size", member.value("size")}, {"sha256", member.value("sha256")},
            {"mode", 0}, {"mtime_ns", "0"}});
    }
    document["entries"] = entries;
    document["archives"] = QJsonArray{descriptor};
    return IncrementalManifest::validate(json(document), request.scan.setId, request.snapshotId, error);
}

enum class Health { Good, Damaged, Unknown };
Health health(Store &store, BackupProvider &provider, const QJsonObject &archive, bool full)
{
    const QString relative = archive.value("remote").toString();
    const auto found = store.lookup(relative);
    if (found == RemoteLookup::Unknown) return Health::Unknown;
    if (found == RemoteLookup::Missing) return Health::Damaged;
    RemoteFile metadata;
    if (!provider.inspect(store.remote(relative), &metadata, &store.error)) return Health::Unknown;
    if (metadata.path != store.remote(relative) || metadata.size < 0) {
        store.error = "Ambiguous remote content metadata.";
        return Health::Unknown;
    }
    if (metadata.size != archive.value("size").toInteger()) return Health::Damaged;
    const QByteArray checksum = QByteArray::fromHex(archive.value("sha256").toString().toLatin1());
    if (provider.trustworthyContentChecksums() && !metadata.checksum.isEmpty()) {
        if (metadata.checksum != checksum) return Health::Damaged;
        if (!full) return Health::Good;
    } else if (!full) return Health::Good; // Explicit daily existence/size-only policy.
    if (!store.verificationSpace(archive)) return Health::Unknown;
    QTemporaryDir directory(QDir(store.staging.path()).filePath("verify-XXXXXX"));
    if (!directory.isValid()) { store.error = "Waiting for verification staging storage."; return Health::Unknown; }
    const QString path = directory.filePath(QFileInfo(relative).fileName());
    if (!provider.download(store.remote(relative), path, &store.error)) return Health::Unknown;
    if (!BackupArchiveIO::verify(path, metadata.size, checksum)
        || !BackupArchiveIO::verifyInventory(path, inventory(archive), &store.error)) return Health::Damaged;
    return Health::Good;
}

QString contentKey(qint64 size, const QString &checksum) { return QString::number(size) + ':' + checksum; }
QJsonObject logical(const IncrementalPlannedFile &file, const QString &archive, const QString &member)
{
    return {{"restore", file.restorePath}, {"archive", archive}, {"member", member}, {"size", file.size},
        {"sha256", QString::fromLatin1(file.checksum.toHex())}, {"mode", file.mode}, {"mtime_ns", file.mtimeNs}};
}

qint64 allowance(qint64 bytes, qint64 count) { return bytes + bytes / 100 + count * 8192 + 65536; }

bool canonicalTime(const QJsonValue &value)
{
    const QString text = value.toString();
    const auto time = QDateTime::fromString(text, Qt::ISODateWithMs);
    return value.isString() && text.size() == 24 && time.isValid() && time.toUTC().toString(Qt::ISODateWithMs) == text;
}

bool validCompletion(const QJsonObject &record, const QString &operationId, const QString &savedSnapshotId, QString *error)
{
    const QStringList fields{"version", "operation_id", "state", "snapshot_id", "checked_at", "last_saved_change"};
    bool valid = record.size() == fields.size();
    for (const auto &field : fields) valid = valid && record.contains(field);
    const QString state = record.value("state").toString();
    valid = valid && record.value("version") == QJsonValue(1) && record.value("operation_id") == QJsonValue(operationId)
        && (state == "saved" || state == "unchanged") && IncrementalManifest::validId(record.value("snapshot_id").toString())
        && (state != "saved" || record.value("snapshot_id").toString() == savedSnapshotId)
        && canonicalTime(record.value("checked_at")) && canonicalTime(record.value("last_saved_change"))
        && record.value("checked_at").toString() >= record.value("last_saved_change").toString();
    if (!valid) *error = "Malformed or inconsistent terminal completion record.";
    return valid;
}

bool completionMatchesHistory(const QJsonObject &record, const QMap<QString, QJsonObject> &history, QString *error)
{
    const auto manifest = history.constFind(record.value("snapshot_id").toString());
    if (manifest == history.cend() || manifest->value("created_at") != record.value("last_saved_change")) {
        *error = "Terminal completion does not match its saved snapshot/timestamp.";
        return false;
    }
    return true;
}

bool validPriorIdentity(const QJsonObject &record, const QString &operationId, const IncrementalWriteRequest &request)
{
    const qint64 epoch = record.value("ownership_epoch").toInteger(-1);
    return record.size() == 10 && record.value("version") == QJsonValue(1) && record.value("payload_format") == QJsonValue(4)
        && record.value("operation_id") == QJsonValue(operationId) && record.value("remote_root") == QJsonValue(request.remoteSetRoot)
        && record.value("set_id") == QJsonValue(request.scan.setId) && record.value("origin_computer_id") == QJsonValue(request.originComputerId)
        && IncrementalManifest::validId(record.value("owner_computer_id").toString())
        && IncrementalManifest::validId(record.value("snapshot_id").toString()) && canonicalTime(record.value("created_at"))
        && epoch > 0 && epoch <= request.ownershipEpoch && record.value("ownership_epoch").toDouble() == double(epoch);
}
} // namespace

bool IncrementalRepository::initialize(const IncrementalWriteRequest &request, BackupProvider &provider,
    bool singleWriterConfirmed, QString *error)
{
    QString reason;
    if (!error) error = &reason;
    if (!singleWriterConfirmed) { *error = "Explicit single-writer/offline-handoff acknowledgement is required."; return false; }
    Store store(request, provider);
    if (!store.open()) { *error = store.error; return false; }
    provider.beginBackupOperation();
    const auto end = qScopeGuard([&] { provider.endBackupOperation(); });
    if (!provider.ensureDirectory(request.remoteSetRoot, &store.error)) { *error = store.error; return false; }
    QStringList existing;
    if (!store.children({}, &existing)) { *error = store.error; return false; }
    if (!existing.isEmpty()) {
        if (!existing.contains("repository.json") || !store.owner()) {
            *error = store.error.isEmpty() ? "Refusing to adopt a nonempty or foreign repository." : store.error;
            return false;
        }
    } else if (!store.put("repository.json", anchor(request), true)) { *error = store.error; return false; }
    for (const QString &directory : {QString("objects"), QString("snapshots"), QString("operations")}) {
        if (!store.owner() || !provider.ensureDirectory(store.remote(directory), &store.error)) { *error = store.error; return false; }
    }
    error->clear();
    return true;
}

IncrementalWriteResult IncrementalRepository::execute(const IncrementalWriteRequest &request, BackupProvider &provider)
{
    IncrementalWriteResult result;
    result.lastSuccessfulCheck = request.scan.lastSuccessfulCheck;
    Store store(request, provider);
    const auto fail = [&](const QString &phase, bool permanent = false) {
        result.state = permanent ? IncrementalWriteResult::State::Failed
            : store.error.contains("staging", Qt::CaseInsensitive) || store.error.contains("storage", Qt::CaseInsensitive)
                || store.error.contains("quota", Qt::CaseInsensitive) ? IncrementalWriteResult::State::Waiting
                : IncrementalWriteResult::State::Retrying;
        result.issues.append({request.remoteSetRoot, phase, store.error.isEmpty() ? "Incremental repository operation failed." : store.error});
        if (!result.manifestCommitted && request.checkpoint
            && request.checkpoint("failure") == IncrementalControl::Pause) result.state = IncrementalWriteResult::State::Paused;
        return result;
    };
    if (!store.open()) return fail("coordination");
    provider.beginBackupOperation();
    const auto end = qScopeGuard([&] { provider.endBackupOperation(); });
    if (!store.owner()) return fail("ownership");
    const QString operation = "operations/" + request.operationId;
    const QString snapshot = "snapshots/" + request.snapshotId + "/manifest.json";
    const auto applyControl = [&](IncrementalControl control) {
        if (control == IncrementalControl::Continue) return true;
        if (result.manifestCommitted) return true; // Commit has won; never claim cancellation removed a snapshot.
        if (control == IncrementalControl::Pause) result.state = IncrementalWriteResult::State::Paused;
        else {
            const QJsonObject cancelled{{"version", 1}, {"operation_id", request.operationId}, {"state", "cancelled"}};
            if (!store.local("cancelled.json", cancelled) || !store.put(operation + "/cancelled.json", cancelled)) {
                result = fail("cancel-checkpoint");
                return false;
            }
            result.state = IncrementalWriteResult::State::Cancelled;
        }
        return false;
    };
    const auto checkpoint = [&](const QString &phase) {
        auto control = request.checkpoint ? request.checkpoint(phase) : IncrementalControl::Continue;
        if (request.scan.stopped && request.scan.stopped()) control = IncrementalControl::Pause;
        return applyControl(control);
    };
    // Never allow two resumable attempts to silently branch a set's latest state.
    QStringList operations;
    QVector<QJsonObject> previousCompletions;
    if (!store.children("operations", &operations)) return fail("operation-discovery");
    for (const auto &name : operations) {
        if (!IncrementalManifest::validId(name)) { store.error = "Unknown operation identity blocks writes."; return fail("operation-discovery"); }
        if (name == request.operationId) continue;
        const auto complete = store.lookup("operations/" + name + "/complete.json");
        const auto cancelled = store.lookup("operations/" + name + "/cancelled.json");
        if (complete == RemoteLookup::Unknown || cancelled == RemoteLookup::Unknown) return fail("operation-discovery");
        if (complete != RemoteLookup::Found && cancelled != RemoteLookup::Found) {
            store.error = "Resume or explicitly cancel the existing protected operation before starting another: " + name;
            return fail("operation-discovery");
        }
        QJsonObject terminal;
        const QString kind = complete == RemoteLookup::Found ? "complete.json" : "cancelled.json";
        if (complete == RemoteLookup::Found && cancelled == RemoteLookup::Found) {
            store.error = "An operation cannot be both completed and cancelled."; return fail("operation-discovery");
        }
        if (!store.get("operations/" + name + '/' + kind, &terminal)) return fail("operation-discovery");
        if (kind == "cancelled.json" && terminal != QJsonObject{{"version", 1}, {"operation_id", name}, {"state", "cancelled"}}) {
            store.error = "Invalid terminal operation record blocks writes."; return fail("operation-discovery");
        }
        if (kind == "complete.json") {
            QJsonObject priorIdentity;
            if (!store.get("operations/" + name + "/identity.json", &priorIdentity)
                || !validPriorIdentity(priorIdentity, name, request)
                || !validCompletion(terminal, name, priorIdentity.value("snapshot_id").toString(), &store.error)
                || (terminal.value("state") == QJsonValue("saved") && terminal.value("last_saved_change") != priorIdentity.value("created_at"))) {
                if (store.error.isEmpty()) store.error = "Invalid completed operation identity.";
                return fail("operation-discovery");
            }
            previousCompletions.append(terminal);
        }
    }
    QJsonObject identity{{"version", 1}, {"payload_format", 4}, {"remote_root", request.remoteSetRoot},
        {"set_id", request.scan.setId}, {"owner_computer_id", request.computerId}, {"origin_computer_id", request.originComputerId},
        {"ownership_epoch", request.ownershipEpoch}, {"operation_id", request.operationId}, {"snapshot_id", request.snapshotId},
        {"created_at", request.scan.checkedAt.toUTC().toString(Qt::ISODateWithMs)}};
    if (!operations.contains(request.operationId)) {
        // A successful complete parent listing, not an inspection/network error,
        // established that this operation namespace does not exist yet.
        if (!store.owner() || !provider.ensureDirectory(store.remote(operation), &store.error)) return fail("identity");
    }
    const auto existingIdentity = store.lookup(operation + "/identity.json");
    if (existingIdentity == RemoteLookup::Unknown) return fail("identity");
    QFile localIdentity(QDir(store.journal).filePath("identity.json"));
    QJsonObject localHeader;
    if (localIdentity.exists()) {
        if (!localIdentity.open(QIODevice::ReadOnly)) { store.error = "Local continuation identity is unreadable."; return fail("identity", true); }
        const auto document = QJsonDocument::fromJson(localIdentity.readAll());
        if (localIdentity.error() != QFileDevice::NoError || !document.isObject()) { store.error = "Local continuation identity is malformed."; return fail("identity", true); }
        localHeader = document.object();
    }
    QJsonObject recorded = localHeader;
    if (existingIdentity == RemoteLookup::Found) {
        if (!store.get(operation + "/identity.json", &recorded)) return fail("identity");
    }
    if (!recorded.isEmpty()) {
        identity["created_at"] = recorded.value("created_at");
        if (identity != recorded || !canonicalTime(recorded.value("created_at"))) {
            store.error = "Continuation belongs to another owner, epoch, set, operation or snapshot.";
            return fail("identity", true);
        }
    }
    if (!localHeader.isEmpty() && localHeader != identity) {
        store.error = "Local continuation identity conflicts with the remote operation.";
        return fail("identity", true);
    }
    if (!store.local("identity.json", identity) || !store.put(operation + "/identity.json", identity)) return fail("identity");
    const auto cancelled = store.lookup(operation + "/cancelled.json");
    if (cancelled == RemoteLookup::Unknown) return fail("cancellation");
    QJsonObject ownCompletion;
    const auto ownCompleteFound = store.lookup(operation + "/complete.json");
    if (ownCompleteFound == RemoteLookup::Unknown) return fail("success-recovery");
    if (ownCompleteFound == RemoteLookup::Found) {
        if (cancelled == RemoteLookup::Found || !store.get(operation + "/complete.json", &ownCompletion)
            || !validCompletion(ownCompletion, request.operationId, request.snapshotId, &store.error)) {
            if (store.error.isEmpty()) store.error = "Invalid or conflicting terminal operation records.";
            return fail("success-recovery");
        }
    }
    if (cancelled == RemoteLookup::Found) {
        QJsonObject marker;
        if (!store.get(operation + "/cancelled.json", &marker) || marker != QJsonObject{{"version", 1}, {"operation_id", request.operationId}, {"state", "cancelled"}}) {
            store.error = "Invalid cancellation checkpoint."; return fail("cancellation");
        }
        result.state = IncrementalWriteResult::State::Cancelled; return result;
    }

    // Read all published history, including manifests that no longer match local
    // configuration. Unknown versions or unreadable history fail closed.
    QMap<QString, QJsonObject> history;
    QMap<QString, QJsonObject> historicalArchives;
    QString latestId;
    QDateTime latestTime;
    QStringList snapshots;
    if (!store.children("snapshots", &snapshots)) return fail("history");
    for (const auto &id : snapshots) {
        if (!IncrementalManifest::validId(id)) { store.error = "Unknown snapshot identity blocks writes."; return fail("history"); }
        const QString path = "snapshots/" + id + "/manifest.json";
        const auto found = store.lookup(path);
        if (found == RemoteLookup::Unknown) return fail("history");
        if (found == RemoteLookup::Missing) continue; // Unpublished namespace.
        QJsonObject manifest;
        if (!store.get(path, &manifest) || !IncrementalManifest::validate(json(manifest), request.scan.setId, id, &store.error)) return fail("history");
        if (manifest.value("origin_computer_id").toString() != request.originComputerId) { store.error = "Foreign snapshot origin."; return fail("history"); }
        for (const auto &value : manifest.value("archives").toArray()) {
            const auto archive = value.toObject();
            const QString objectId = archive.value("id").toString();
            if (historicalArchives.contains(objectId) && historicalArchives.value(objectId) != archive) {
                store.error = "Conflicting immutable object descriptors in saved history."; return fail("history");
            }
            historicalArchives.insert(objectId, archive);
        }
        history.insert(id, manifest);
        const auto time = QDateTime::fromString(manifest.value("created_at").toString(), Qt::ISODateWithMs);
        if (!latestTime.isValid() || time > latestTime || (time == latestTime && id > latestId)) { latestTime = time; latestId = id; }
    }
    for (const auto &completed : previousCompletions)
        if (!completionMatchesHistory(completed, history, &store.error)) return fail("operation-discovery");
    if (!ownCompletion.isEmpty() && !completionMatchesHistory(ownCompletion, history, &store.error)) return fail("success-recovery");
    result.lastSavedChange = latestTime;
    const auto noteDamage = [&](const QJsonObject &archive) {
        const QString id = archive.value("id").toString();
        if (!result.damagedObjects.contains(id)) result.damagedObjects.append(id);
        for (auto snapshot = history.cbegin(); snapshot != history.cend(); ++snapshot)
            for (const auto &value : snapshot.value().value("archives").toArray())
                if (value.toObject().value("id").toString() == id && !result.affectedSnapshots.contains(snapshot.key()))
                    result.affectedSnapshots.append(snapshot.key());
        result.damagedObjects.sort();
        result.affectedSnapshots.sort();
    };
    const auto finish = [&](const QJsonObject &manifest, bool unchanged) {
        result.manifestCommitted = !unchanged;
        result.manifestPath = unchanged ? QString() : store.remote(snapshot);
        const auto saved = unchanged ? latestTime : QDateTime::fromString(manifest.value("created_at").toString(), Qt::ISODateWithMs);
        QJsonObject completed{{"version", 1}, {"operation_id", request.operationId},
            {"state", unchanged ? "unchanged" : "saved"}, {"snapshot_id", unchanged ? latestId : request.snapshotId},
            {"checked_at", request.scan.checkedAt.toUTC().toString(Qt::ISODateWithMs)},
            {"last_saved_change", saved.isValid() ? saved.toUTC().toString(Qt::ISODateWithMs) : QString()}};
        const auto existing = store.lookup(operation + "/complete.json");
        if (existing == RemoteLookup::Unknown) return fail("success-checkpoint");
        if (existing == RemoteLookup::Found) {
            QJsonObject recorded;
            if (!store.get(operation + "/complete.json", &recorded)) return fail("success-checkpoint");
            if (!validCompletion(recorded, request.operationId, request.snapshotId, &store.error)) return fail("success-checkpoint");
            const QString checked = recorded.value("checked_at").toString();
            completed["checked_at"] = checked;
            if (completed != recorded || !QDateTime::fromString(checked, Qt::ISODateWithMs).isValid()) {
                store.error = "Conflicting immutable completion record."; return fail("success-checkpoint");
            }
        }
        if (!validCompletion(completed, request.operationId, request.snapshotId, &store.error)) return fail("success-checkpoint");
        if (!store.local("result.json", completed) || !store.put(operation + "/complete.json", completed)) return fail("success-checkpoint");
        result.lastSuccessfulCheck = QDateTime::fromString(completed.value("checked_at").toString(), Qt::ISODateWithMs);
        result.lastSavedChange = saved;
        result.state = unchanged ? IncrementalWriteResult::State::Unchanged : IncrementalWriteResult::State::Saved;
        return result;
    };

    // Publication intent fixes the final manifest across lost responses and local
    // journal loss. Reconcile it before looking at changed current source files.
    QJsonObject publication;
    const auto intentFound = store.lookup(operation + "/publication.json");
    if (intentFound == RemoteLookup::Unknown) return fail("publication-recovery");
    if (intentFound == RemoteLookup::Found) {
        if (!store.get(operation + "/publication.json", &publication)
            || !IncrementalManifest::validate(json(publication), request.scan.setId, request.snapshotId, &store.error)) return fail("publication-recovery");
        if (publication.value("owner_computer_id").toString() != request.computerId
            || publication.value("ownership_epoch").toInteger() != request.ownershipEpoch
            || publication.value("origin_computer_id").toString() != request.originComputerId
            || publication.value("created_at") != identity.value("created_at")) {
            store.error = "Publication intent belongs to another owner/epoch."; return fail("publication-recovery");
        }
        if (history.contains(request.snapshotId)) {
            if (history.value(request.snapshotId) != publication) { store.error = "Published snapshot conflicts with its immutable intent."; return fail("publication-recovery", true); }
            result.manifestCommitted = true;
        }
        if (!checkpoint("publication-resume")) return result;
        for (const auto &value : publication.value("archives").toArray()) {
            const auto state = health(store, provider, value.toObject(), false);
            if (state == Health::Unknown) return fail("publication-recovery");
            if (state == Health::Damaged) { noteDamage(value.toObject()); store.error = "Publication references damaged data; start a new operation, never rewrite its immutable intent."; return fail("publication-recovery", true); }
        }
        if (!store.put(snapshot, publication)) return fail("publication-recovery");
        result.manifestCommitted = true;
        return finish(publication, false);
    }
    if (history.contains(request.snapshotId)) { store.error = "Snapshot ID is already published without this operation's intent."; return fail("identity", true); }
    if (!ownCompletion.isEmpty()) {
        if (ownCompletion.value("state") != QJsonValue("unchanged")) {
            store.error = "Invalid terminal unchanged-check record."; return fail("success-recovery");
        }
        result.lastSuccessfulCheck = QDateTime::fromString(ownCompletion.value("checked_at").toString(), Qt::ISODateWithMs);
        result.lastSavedChange = QDateTime::fromString(ownCompletion.value("last_saved_change").toString(), Qt::ISODateWithMs);
        result.state = IncrementalWriteResult::State::Unchanged;
        return result;
    }
    if (!checkpoint("started")) return result;
    IncrementalPlanRequest scan = request.scan;
    if (!latestId.isEmpty()) {
        if ((!scan.previousSnapshotId.isEmpty() && scan.previousSnapshotId != latestId)
            || (!scan.previousManifest.isEmpty() && QJsonDocument::fromJson(scan.previousManifest).object() != history.value(latestId))) {
            store.error = "Supplied previous snapshot is not the latest saved state."; return fail("planning", true);
        }
        scan.previousSnapshotId = latestId;
        scan.previousManifest = json(history.value(latestId));
    } else if (!scan.previousSnapshotId.isEmpty() || !scan.previousManifest.isEmpty()) {
        store.error = "Expected history is absent; refusing a silent new baseline."; return fail("planning", true);
    }
    IncrementalControl activeControl = IncrementalControl::Continue;
    scan.stopped = [&] {
        if (request.scan.stopped && request.scan.stopped()) { activeControl = IncrementalControl::Pause; return true; }
        activeControl = request.checkpoint ? request.checkpoint("capturing") : IncrementalControl::Continue;
        return activeControl != IncrementalControl::Continue;
    };
    const auto plan = IncrementalPlanner::plan(scan);
    if (!plan.success) {
        if (activeControl != IncrementalControl::Continue) { applyControl(activeControl); return result; }
        result.issues = plan.issues;
        store.error = "Source planning failed; no snapshot was published.";
        return fail("planning", true);
    }
    if (!checkpoint("planned")) return result;

    QMap<QString, QJsonObject> available;
    QSet<QString> originallyRequired;
    bool repairRequired = false;
    for (const auto &value : plan.requiredArchives) {
        const auto archive = value.toObject();
        const QString id = archive.value("id").toString();
        originallyRequired.insert(id);
        available.insert(id, archive);
    }
    // Recover per-object records from remote, not just local journals. Prepared
    // objects need a full hash/physical-inventory check before obtaining evidence.
    QStringList records;
    if (!store.children(operation, &records)) return fail("continuation");
    const QSet<QString> recordNames(records.cbegin(), records.cend());
    QSet<QString> recoveredObjects;
    for (const auto &name : records) {
        if (name == "identity.json" || name == "publication.json" || name == "complete.json" || name == "cancelled.json") continue;
        const QString prefix = name.startsWith("prepared-") ? "prepared-" : name.startsWith("verified-") ? "verified-" : name.startsWith("reuse-") ? "reuse-" : QString();
        const QString id = name.mid(prefix.size(), name.size() - prefix.size() - 5);
        if (prefix.isEmpty() || !IncrementalManifest::validId(id) || name != prefix + id + ".json"
            || (prefix == "verified-" && !recordNames.contains("prepared-" + id + ".json"))) {
            store.error = "Unknown or orphaned operation checkpoint blocks writes."; return fail("continuation");
        }
        if (prefix == "reuse-") {
            QJsonObject reused;
            if (!store.get(operation + '/' + name, &reused) || reused.value("version") != QJsonValue(1) || !reused.value("archives").isArray()) return fail("reuse-protection");
            for (const auto &value : reused.value("archives").toArray())
                if (!validArchive(value.toObject(), true, request, &store.error)) return fail("reuse-protection");
        }
    }
    for (const auto &name : records) {
        if (!name.startsWith("prepared-")) continue;
        const QString id = name.mid(9, name.size() - 14);
        if (!IncrementalManifest::validId(id) || name != "prepared-" + id + ".json") { store.error = "Malformed prepared object identity."; return fail("continuation"); }
        QJsonObject record;
        if (!store.get(operation + '/' + name, &record) || record.value("version") != QJsonValue(1)
            || !validArchive(record.value("archive").toObject(), false, request, &store.error)) return fail("continuation");
        QJsonObject archive = record.value("archive").toObject();
        if (archive.value("id").toString() != id) { store.error = "Prepared object identity mismatch."; return fail("continuation"); }
        const QString verifiedName = "verified-" + id + ".json";
        const auto verifiedFound = store.lookup(operation + '/' + verifiedName);
        if (verifiedFound == RemoteLookup::Unknown) return fail("continuation");
        bool verified = verifiedFound == RemoteLookup::Found;
        if (verified) {
            QJsonObject evidence;
            if (!store.get(operation + '/' + verifiedName, &evidence) || evidence.value("version") != QJsonValue(1)
                || !validArchive(evidence.value("archive").toObject(), true, request, &store.error)) return fail("continuation");
            QJsonObject physical = evidence.value("archive").toObject();
            physical.remove("original_verification");
            if (physical != archive) { store.error = "Conflicting prepared and verified object descriptors."; return fail("continuation"); }
            archive = evidence.value("archive").toObject();
        }
        const auto objectFound = store.lookup(archive.value("remote").toString());
        if (objectFound == RemoteLookup::Unknown) return fail("continuation-verification");
        if (objectFound == RemoteLookup::Missing) continue; // Prepared does not mean it was ever uploaded.
        const auto state = health(store, provider, archive, !verified);
        if (state == Health::Unknown) return fail("continuation-verification");
        if (state == Health::Damaged) { noteDamage(archive); continue; }
        if (!verified) {
            archive["original_verification"] = QJsonObject{{"method", "download-sha256"},
                {"verified_at", request.scan.checkedAt.toUTC().toString(Qt::ISODateWithMs)}, {"sha256", archive.value("sha256")}};
            const QJsonObject evidence{{"version", 1}, {"archive", archive}};
            if (!store.put(operation + '/' + verifiedName, evidence) || !store.local(verifiedName, evidence)) return fail("verified-checkpoint");
        }
        if (available.contains(id) && available.value(id) != archive) { store.error = "Conflicting continuation object descriptor."; return fail("continuation"); }
        available.insert(id, archive);
        recoveredObjects.insert(id);
    }
    for (auto item = available.begin(); item != available.end();) {
        const auto state = health(store, provider, item.value(), false);
        if (state == Health::Unknown) return fail("reuse-check");
        if (state == Health::Damaged) {
            repairRequired = repairRequired || originallyRequired.contains(item.key());
            noteDamage(item.value()); item = available.erase(item);
        }
        else ++item;
    }
    // Protection is durable before any reused reference is claimed by this run.
    QJsonArray protectedArchives;
    for (const auto &archive : available) protectedArchives.append(archive);
    const QJsonObject reused{{"version", 1}, {"archives", protectedArchives}};
    const QString reuseRecord = "reuse-" + uuid() + ".json";
    if (!store.local(reuseRecord, reused) || !store.put(operation + '/' + reuseRecord, reused)) return fail("reuse-protection");
    if (!checkpoint("reuse-checked")) return result;

    QMap<QString, QPair<QString, QString>> matches;
    QSet<QString> previousMembers;
    for (const auto &value : history.value(latestId).value("entries").toArray()) {
        const auto entry = value.toObject();
        previousMembers.insert(entry.value("archive").toString() + QChar::Null + entry.value("member").toString());
    }
    for (const auto &archive : available) {
        // Previous snapshot reuse is limited to its logical references; only
        // this operation's recovered objects may use their complete inventory.
        for (const auto &value : archive.value("members").toArray()) {
            const auto member = value.toObject();
            const QString id = archive.value("id").toString();
            const bool allowed = recoveredObjects.contains(id)
                || previousMembers.contains(id + QChar::Null + member.value("path").toString());
            if (!allowed) continue;
            const QString key = contentKey(member.value("size").toInteger(), member.value("sha256").toString());
            const QPair<QString, QString> candidate{id, member.value("path").toString()};
            if (!matches.contains(key) || candidate < matches.value(key)) matches.insert(key, candidate);
        }
    }
    QMap<QString, QJsonObject> logicalEntries;
    QVector<IncrementalPlannedFile> uploads;
    for (const auto &file : plan.files) {
        const auto match = matches.constFind(contentKey(file.size, QString::fromLatin1(file.checksum.toHex())));
        if (match != matches.cend()) logicalEntries.insert(file.restorePath, logical(file, match->first, match->second));
        else uploads.append(file);
    }
    const bool unchanged = !plan.snapshotRequired && uploads.isEmpty() && !repairRequired;
    if (unchanged) return finish({}, true);

    for (qsizetype first = 0; first < uploads.size();) {
        QVector<IncrementalPlannedFile> group;
        qint64 bytes = 0;
        do {
            const auto &file = uploads[first];
            const qint64 proposed = bytes + file.size;
            if (!group.isEmpty() && (group.size() >= request.batchFileLimit || proposed > request.archiveTargetBytes
                || proposed + 2 * allowance(proposed, group.size() + 1) > request.stagingBudget)) break;
            group.append(file); bytes = proposed; ++first;
        } while (first < uploads.size());
        // The only soft-budget exception is one whole oversized file, as in v3.
        const qint64 archiveAllowance = allowance(bytes, group.size());
        if (bytes + 2 * archiveAllowance > request.stagingBudget && !checkpoint("preparing-oversized-file")) return result;
        if (!store.staging.hasSpace(bytes + 2 * archiveAllowance, &store.error)) return fail("staging-preflight");
        if (!store.staging.reset(&store.error)) return fail("staging-release");
        const QString payloads = QDir(store.staging.path()).filePath("payloads");
        const QString tree = QDir(payloads).filePath("tree");
        if (!QDir().mkpath(tree)) { store.error = "Waiting for staging storage."; return fail("staging"); }
        QVector<BackupEntry> entries;
        int index = 0;
        const auto stopped = [&] {
            activeControl = request.checkpoint ? request.checkpoint("preparing") : IncrementalControl::Continue;
            if (request.scan.stopped && request.scan.stopped()) activeControl = IncrementalControl::Pause;
            return activeControl != IncrementalControl::Continue;
        };
        for (const auto &file : group) {
            const QString member = "file-" + QString::number(index++);
            if (!IncrementalPlanner::stage(file, QDir(tree).filePath(member), &store.error, stopped)) {
                if (activeControl != IncrementalControl::Continue) { applyControl(activeControl); return result; }
                return fail("source-capture", !store.error.startsWith("Waiting"));
            }
            entries.append({file.sourcePath, "payload/" + member, file.size, file.checksum, file.restorePath});
        }
        if (!checkpoint("prepared-sources")) return result;
        const QString id = uuid();
        const QString archivePath = QDir(payloads).filePath(id + ".tar.gz");
        if (!BackupArchiveIO::pack(tree, "payload", archivePath, archiveAllowance, &entries, &store.error, stopped)) {
            if (activeControl != IncrementalControl::Continue) { applyControl(activeControl); return result; }
            store.error = "Waiting for archive staging storage: " + store.error; return fail("compression");
        }
        QFile archiveFile(archivePath);
        if (!archiveFile.open(QIODevice::ReadOnly)) { store.error = archiveFile.errorString(); return fail("archive-hash"); }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&archiveFile)) { store.error = "Unable to hash prepared archive."; return fail("archive-hash"); }
        const qint64 compressedSize = archiveFile.size();
        archiveFile.close();
        if (!archiveFile.setPermissions(QFileDevice::ReadOwner)) { store.error = "Unable to protect the prepared archive."; return fail("archive-hash"); }
        QJsonArray members;
        for (const auto &entry : entries) members.append(QJsonObject{{"path", entry.memberPath}, {"size", entry.size}, {"sha256", QString::fromLatin1(entry.checksum.toHex())}});
        QJsonObject archive{{"id", id}, {"remote", "objects/" + id + ".tar.gz"}, {"size", compressedSize},
            {"sha256", QString::fromLatin1(hash.result().toHex())}, {"members", members}};
        if (!validArchive(archive, false, request, &store.error)
            || !BackupArchiveIO::verifyInventory(archivePath, inventory(archive), &store.error)) return fail("archive-validation", true);
        const QString prepared = "prepared-" + id + ".json";
        const QJsonObject preparation{{"version", 1}, {"archive", archive}};
        if (!store.local(prepared, preparation)) return fail("prepared-checkpoint");
        if (!checkpoint("prepared-local")) return result;
        if (!store.put(operation + '/' + prepared, preparation)) return fail("remote-protection");
        if (!checkpoint("protected")) return result;
        if (!store.owner()) return fail("ownership");
        const auto absent = store.lookup(archive.value("remote").toString());
        if (absent == RemoteLookup::Unknown) return fail("upload-lookup");
        if (absent != RemoteLookup::Missing) { store.error = "Fresh object ID unexpectedly exists; refusing overwrite."; return fail("upload", true); }
        if (!provider.uploadImmutable(archivePath, store.remote(archive.value("remote").toString()), &store.error)) return fail("upload");
        if (!checkpoint("uploaded")) return result;
        // Keep source captures AND compressed output through verification and the
        // durable verified checkpoint. Budget includes a second compressed download.
        const auto verified = health(store, provider, archive, true);
        if (verified == Health::Unknown) return fail("verification");
        if (verified == Health::Damaged) { noteDamage(archive); store.error = "New upload failed full verification; immutable data will not be overwritten."; return fail("verification"); }
        if (!checkpoint("object-verified")) return result;
        archive["original_verification"] = QJsonObject{{"method", "download-sha256"},
            {"verified_at", request.scan.checkedAt.toUTC().toString(Qt::ISODateWithMs)}, {"sha256", archive.value("sha256")}};
        const QString verifiedName = "verified-" + id + ".json";
        const QJsonObject evidence{{"version", 1}, {"archive", archive}};
        if (!store.put(operation + '/' + verifiedName, evidence)) return fail("remote-evidence");
        if (!checkpoint("verified-remote")) return result;
        if (!store.local(verifiedName, evidence)) return fail("verified-checkpoint");
        if (!checkpoint("checkpointed")) return result;
        available.insert(id, archive);
        for (int index = 0; index < group.size(); ++index)
            logicalEntries.insert(group[index].restorePath, logical(group[index], id, entries[index].memberPath));
        if (!store.staging.reset(&store.error)) return fail("staging-release");
    }
    QSet<QString> used;
    QJsonArray entries, archives;
    for (const auto &entry : logicalEntries) { entries.append(entry); used.insert(entry.value("archive").toString()); }
    for (const auto &archive : available) if (used.contains(archive.value("id").toString())) archives.append(archive);
    // Recheck all reused references immediately before final publication.
    for (const auto &value : archives) {
        const auto state = health(store, provider, value.toObject(), false);
        if (state == Health::Unknown) return fail("final-reuse-check");
        if (state == Health::Damaged) { noteDamage(value.toObject()); store.error = "Remote object changed before publication; retry with replacement content."; return fail("final-reuse-check"); }
    }
    publication = manifestBase(request, QDateTime::fromString(identity.value("created_at").toString(), Qt::ISODateWithMs));
    publication["archives"] = archives;
    publication["entries"] = entries;
    if (!IncrementalManifest::validate(json(publication), request.scan.setId, request.snapshotId, &store.error)) return fail("manifest-validation", true);
    if (!store.local("publication.json", publication) || !store.put(operation + "/publication.json", publication)) return fail("publication-intent");
    if (!checkpoint("publication-prepared")) return result;
    if (!store.put(snapshot, publication)) return fail("publication");
    result.manifestCommitted = true;
    if (!checkpoint("published")) return result;
    return finish(publication, false);
}

#include "incrementalrepository.h"
#include "incrementalmanifest.h"
#include "backuparchive.h"
#include "backupstaging.h"
#include "backupcatalog.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
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
#include <algorithm>

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
         || request.archiveTargetBytes <= 0 || request.archiveTargetBytes > 9007199254740991LL || request.batchFileLimit <= 0 || request.retention < 0
         || request.integrityByteBudget <= 0 || request.integrityTimeBudgetMs <= 0) {
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
    QString repositoryJournal;
    std::unique_ptr<QLockFile> lock;
    bool completeListings = false;
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
        repositoryJournal = setJournal;
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
        if (!(completeListings ? provider.listComplete(remote(relative), &items, &error)
                              : provider.list(remote(relative), &items, &error))) return false;
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
bool persistDamage(Store &store, QJsonObject archive, const QDateTime &detectedAt)
{
    archive.remove("original_verification");
    const QString id = archive.value("id").toString();
    const QString relative = "integrity-damage/" + id + ".json";
    QJsonObject record{{"version", 1}, {"archive", archive}, {"detected_at", detectedAt.toUTC().toString(Qt::ISODateWithMs)}};
    // A local checkpoint comes first so a failed remote publication can be
    // recovered independently of the writer operation/cycle ID.
    const QString path = QDir(store.repositoryJournal).filePath("integrity-damage-" + id + ".json");
    QFile existing(path);
    if (existing.exists()) {
        if (QFileInfo(existing).isSymLink() || !existing.open(QIODevice::ReadOnly)) return false;
        const auto document = QJsonDocument::fromJson(existing.readAll());
        const auto previous = document.object();
        if (existing.error() != QFileDevice::NoError || !document.isObject() || previous.size() != 3
            || previous.value("version") != QJsonValue(1) || previous.value("archive") != archive
            || !QDateTime::fromString(previous.value("detected_at").toString(), Qt::ISODateWithMs).isValid()) return false;
        record = previous;
    }
    return save(path, record, &store.error) && store.put(relative, record);
}

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

// Deletion proposals are immutable, self-contained authorization scopes. Archived
// manifests remain here for validating old completion records, NOT as live refs.
QStringList legacyScopes(const IncrementalWriteRequest &request)
{
    auto roots = request.legacyRoots;
    if (!request.legacyRoot.isEmpty()) roots.append(request.legacyRoot);
    roots.removeDuplicates();
    roots.sort();
    return roots;
}

bool validCleanupProposal(const QJsonObject &proposal, const IncrementalWriteRequest &request, QString *error)
{
    auto originalOwner = request;
    const auto owner = proposal.value("anchor").toObject();
    originalOwner.computerId = owner.value("owner_computer_id").toString();
    originalOwner.ownershipEpoch = owner.value("ownership_epoch").toInteger(0);
    if (proposal.size() != 11 || proposal.value("version") != QJsonValue(1)
        || !IncrementalManifest::validId(originalOwner.computerId) || originalOwner.ownershipEpoch < 1
        || originalOwner.ownershipEpoch > request.ownershipEpoch || proposal.value("anchor") != anchor(originalOwner)
        || (originalOwner.ownershipEpoch == request.ownershipEpoch && originalOwner.computerId != request.computerId)
        || !IncrementalManifest::validId(proposal.value("id").toString())
        || proposal.value("retention").toInt(0) < 1 || !proposal.value("snapshots").isArray()
        || !proposal.value("objects").isArray() || !proposal.value("legacy").isArray()
        || !proposal.value("generation").isString() || !proposal.value("identities").isObject()
        || !proposal.value("estimated_live_member_bytes").isDouble() || !proposal.value("estimated_obsolete_member_bytes").isDouble()) {
        *error = "Invalid or foreign deletion proposal."; return false;
    }
    QSet<QString> ids;
    for (const auto &value : proposal.value("snapshots").toArray()) {
        const auto manifest = value.toObject();
        const QString id = manifest.value("snapshot_id").toString();
        if (ids.contains(id) || !IncrementalManifest::validate(json(manifest), request.scan.setId, id, error)
            || manifest.value("origin_computer_id") != request.originComputerId) return false;
        ids.insert(id);
    }
    ids.clear();
    for (const auto &value : proposal.value("objects").toArray()) {
        const auto archive = value.toObject();
        const QString id = archive.value("id").toString();
        if (ids.contains(id) || !validArchive(archive, true, request, error)) return false;
        ids.insert(id);
    }
    ids.clear();
    for (const auto &value : proposal.value("legacy").toArray()) {
        const auto copy = value.toObject();
        const QString path = copy.value("path").toString();
        const QString root = QFileInfo(path).path();
        if (copy.size() != 2 || !legacyScopes(request).contains(root) || root.isEmpty() || root != QDir::cleanPath(root) || root == "/" || root == "."
            || root.contains('\\') || root.contains(QChar::Null) || root.split('/').contains("..")
            || path != QDir::cleanPath(path) || QFileInfo(path).path() != root
            || path == request.remoteSetRoot || path.startsWith(request.remoteSetRoot + '/')
            || request.remoteSetRoot.startsWith(path + '/') || ids.contains(path)
            || !copy.value("manifest").isObject()) {
            *error = "Unsafe legacy deletion scope."; return false;
        }
        ids.insert(path);
    }
    QSet<QString> paths;
    for (const auto &value : proposal.value("snapshots").toArray())
        paths.insert(request.remoteSetRoot + "/snapshots/" + value.toObject().value("snapshot_id").toString() + "/manifest.json");
    for (const auto &value : proposal.value("objects").toArray())
        paths.insert(request.remoteSetRoot + '/' + value.toObject().value("remote").toString());
    for (const auto &value : proposal.value("legacy").toArray()) paths.insert(value.toObject().value("path").toString());
    const auto identities = proposal.value("identities").toObject();
    if (identities.size() != paths.size()) { *error = "Deletion identity scope mismatch."; return false; }
    for (const auto &path : paths) if (!identities.value(path).isString() || identities.value(path).toString().isEmpty()) {
        *error = "Missing stable deletion identity."; return false;
    }
    return true;
}

bool cleanupRecords(Store &store, const IncrementalWriteRequest &request,
    QMap<QString, QJsonObject> *retired, QString *active)
{
    const bool originalListings = store.completeListings;
    store.completeListings = true;
    const auto restoreListings = qScopeGuard([&] { store.completeListings = originalListings; });
    const auto found = store.lookup("maintenance");
    if (found == RemoteLookup::Unknown) return false;
    if (found == RemoteLookup::Missing) return true;
    QStringList names;
    if (!store.children("maintenance", &names)) return false;
    for (const auto &id : names) {
        if (!IncrementalManifest::validId(id)) { store.error = "Unknown maintenance record blocks mutations."; return false; }
        QJsonObject proposal;
        const QString root = "maintenance/" + id;
        const auto published = store.lookup(root + "/proposal.json");
        if (published == RemoteLookup::Unknown) return false;
        if (published == RemoteLookup::Missing) {
            QStringList files;
            if (!store.children(root, &files)) return false;
            // Creating a directory is not publishing deletion authority. A
            // complete fresh inventory must prove it is entirely empty before
            // a failed initial upload may be ignored, even after local loss.
            if (files.isEmpty()) continue;
            store.error = "Unpublished maintenance namespace contains unknown records.";
            return false;
        }
        if (!store.get(root + "/proposal.json", &proposal)
            || !validCleanupProposal(proposal, request, &store.error) || proposal.value("id") != id) return false;
        const auto done = store.lookup(root + "/done.json");
        if (done == RemoteLookup::Unknown) return false;
        const QJsonObject receipt{{"proposal", digest(json(proposal))}};
        QSet<QString> expected{"proposal.json", "authorized.json", "done.json", "dismissed.json"};
        QStringList required{"authorized.json"};
        const auto phases = [&](const QString &key) {
            for (const auto &phase : {QString("trashed"), QString("deleted")}) {
                const QString name = key + '-' + phase + ".json";
                expected.insert(name);
                required.append(name);
            }
        };
        for (const auto &value : proposal.value("snapshots").toArray()) phases("snapshot-" + value.toObject().value("snapshot_id").toString());
        for (const auto &value : proposal.value("objects").toArray()) phases("object-" + value.toObject().value("id").toString());
        for (const auto &value : proposal.value("legacy").toArray()) phases("legacy-" + digest(value.toObject().value("path").toString().toUtf8()));
        QStringList files;
        if (!store.children(root, &files)) return false;
        for (const auto &name : files) {
            if (!expected.contains(name)) { store.error = "Unknown cleanup checkpoint blocks mutations."; return false; }
            if (name == "proposal.json") continue;
            QJsonObject observed;
            if (!store.get(root + '/' + name, &observed) || observed != receipt) {
                store.error = "Unreadable/invalid cleanup checkpoint."; return false;
            }
            if (name.endsWith("-deleted.json") && !files.contains(name.left(name.size() - 13) + "-trashed.json")) {
                store.error = "Deletion phase is out of order."; return false;
            }
            if (name != "authorized.json" && name != "dismissed.json" && !files.contains("authorized.json")) {
                store.error = "Cleanup phase lacks authorization."; return false;
            }
        }
        if (files.contains("dismissed.json")) {
            if (files.size() != 2 || files.contains("authorized.json")) {
                store.error = "A dismissed cleanup proposal has unexpected authorization or mutation records."; return false;
            }
            continue; // No deletions occurred and no snapshots were retired.
        }
        if (done == RemoteLookup::Found) for (const auto &name : required) if (!files.contains(name)) {
            store.error = "Cleanup completion lacks durable target phases."; return false;
        }
        if (done == RemoteLookup::Missing) {
            if (proposal.value("anchor") != anchor(request)) {
                store.error = "Unfinished deletion belongs to an old epoch; renewed authorization/reconciliation is required."; return false;
            }
            if (!active->isEmpty()) { store.error = "Multiple unresolved deletion intents require reconciliation."; return false; }
            *active = id;
        } else {
            QJsonObject marker;
            if (!store.get(root + "/done.json", &marker)
                || marker != QJsonObject{{"proposal", digest(json(proposal))}}) {
                store.error = "Invalid cleanup completion."; return false;
            }
            for (const auto &value : proposal.value("snapshots").toArray()) {
                const auto manifest = value.toObject();
                const QString snapshot = manifest.value("snapshot_id").toString();
                if (retired->contains(snapshot) && retired->value(snapshot) != manifest) {
                    store.error = "Conflicting retired history."; return false;
                }
                retired->insert(snapshot, manifest);
            }
        }
    }
    return true;
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

namespace {
struct IntegrityInventory {
    QMap<QString, QJsonObject> archives;
    QMap<QString, QJsonObject> originalEvidence;
    QMap<QString, QStringList> snapshots;
};

bool integrityInventory(Store &store, const IncrementalWriteRequest &request, IntegrityInventory *inventory)
{
    const auto add = [&](QJsonObject archive, bool verified, const QString &snapshot = QString()) {
        if (!validArchive(archive, verified, request, &store.error)) return false;
        const QString id = archive.value("id").toString();
        if (verified) {
            const auto original = archive.value("original_verification").toObject();
            if (inventory->originalEvidence.contains(id) && inventory->originalEvidence.value(id) != original) {
                store.error = "Conflicting immutable original verification receipts."; return false;
            }
            inventory->originalEvidence.insert(id, original);
        }
        archive.remove("original_verification");
        if (inventory->archives.contains(id) && inventory->archives.value(id) != archive) {
            store.error = "Conflicting immutable integrity descriptors."; return false;
        }
        inventory->archives.insert(id, archive);
        if (!snapshot.isEmpty() && !inventory->snapshots[id].contains(snapshot)) inventory->snapshots[id].append(snapshot);
        return true;
    };
    QStringList snapshots;
    if (!store.children("snapshots", &snapshots)) return false;
    for (const auto &id : snapshots) {
        if (!IncrementalManifest::validId(id)) { store.error = "Unknown integrity snapshot identity."; return false; }
        const QString path = "snapshots/" + id + "/manifest.json";
        const auto found = store.lookup(path);
        if (found == RemoteLookup::Unknown) return false;
        if (found == RemoteLookup::Missing) continue;
        QJsonObject manifest;
        if (!store.get(path, &manifest) || !IncrementalManifest::validate(json(manifest), request.scan.setId, id, &store.error)
            || manifest.value("origin_computer_id") != request.originComputerId) return false;
        for (const auto &archive : manifest.value("archives").toArray()) if (!add(archive.toObject(), true, id)) return false;
    }
    QStringList operations;
    if (!store.children("operations", &operations)) return false;
    for (const auto &id : operations) {
        if (!IncrementalManifest::validId(id)) { store.error = "Unknown protected operation identity."; return false; }
        const QString root = "operations/" + id;
        QJsonObject identity;
        if (!store.get(root + "/identity.json", &identity) || !validPriorIdentity(identity, id, request)) {
            store.error = "Invalid protected operation identity."; return false;
        }
        const auto completed = store.lookup(root + "/complete.json"), cancelled = store.lookup(root + "/cancelled.json");
        if (completed == RemoteLookup::Unknown || cancelled == RemoteLookup::Unknown) return false;
        if (completed == RemoteLookup::Found && cancelled == RemoteLookup::Found) { store.error = "Conflicting operation terminal records."; return false; }
        if (completed == RemoteLookup::Found || cancelled == RemoteLookup::Found) {
            QJsonObject terminal;
            if (!store.get(root + (completed == RemoteLookup::Found ? "/complete.json" : "/cancelled.json"), &terminal)) return false;
            if (completed == RemoteLookup::Found
                ? !validCompletion(terminal, id, identity.value("snapshot_id").toString(), &store.error)
                : terminal != QJsonObject{{"version", 1}, {"operation_id", id}, {"state", "cancelled"}}) return false;
            continue; // Only unfinished operations protect otherwise unreferenced objects.
        }
        QStringList records;
        if (!store.children(root, &records)) return false;
        for (const auto &name : records) {
            if (name == "identity.json") continue;
            QJsonObject record;
            if (!store.get(root + '/' + name, &record)) return false;
            if (name == "publication.json") {
                if (!IncrementalManifest::validate(json(record), request.scan.setId, identity.value("snapshot_id").toString(), &store.error)) return false;
                for (const auto &archive : record.value("archives").toArray()) if (!add(archive.toObject(), true)) return false;
                continue;
            }
            const QString prefix = name.startsWith("prepared-") ? "prepared-" : name.startsWith("verified-") ? "verified-" : name.startsWith("reuse-") ? "reuse-" : QString();
            const QString objectId = name.mid(prefix.size(), name.size() - prefix.size() - 5);
            if (prefix.isEmpty() || !IncrementalManifest::validId(objectId) || name != prefix + objectId + ".json"
                || record.size() != 2 || record.value("version") != QJsonValue(1)) { store.error = "Unknown integrity protection record."; return false; }
            if (prefix == "reuse-") {
                if (!record.value("archives").isArray()) return false;
                for (const auto &archive : record.value("archives").toArray()) if (!add(archive.toObject(), true)) return false;
            } else {
                const auto archive = record.value("archive").toObject();
                if (archive.value("id") != objectId) return false;
                // An upload may not yet exist. Its preparation still belongs in
                // coverage; missing data is never certified or deleted here.
                if (!add(archive, prefix == "verified-")) return false;
            }
        }
    }
    return true;
}

QJsonArray integrityWorklist(const IntegrityInventory &inventory)
{
    QJsonArray objects;
    for (const auto &archive : inventory.archives) objects.append(archive);
    return objects;
}

QString integrityBinding(const QJsonObject &archive, const QString &revision, qint64 size, const QString &run)
{
    return digest(json(QJsonObject{{"archive", archive}, {"revision", revision}, {"size", size}, {"run", run}}));
}

bool validIntegrityReceipt(const QJsonObject &receipt, const QJsonObject &plan,
    const IncrementalWriteRequest &request, QString *error)
{
    const auto archive = receipt.value("archive").toObject();
    const QString revision = receipt.value("revision").toString(), run = receipt.value("run").toString();
    const qint64 size = receipt.value("observed_size").toInteger(-1);
    return receipt.size() == 9 && receipt.value("cycle") == plan.value("id")
        && receipt.value("method") == "download-sha256-inventory" && receipt.value("revision").isString()
        && receipt.value("run").isString() && (revision.isEmpty() ? IncrementalManifest::validId(run) : run.isEmpty())
        && size >= 0 && receipt.value("observed_size") == QJsonValue(size)
        && (receipt.value("state") == "good" || receipt.value("state") == "damaged")
        && (receipt.value("state") != "good" || size == archive.value("size").toInteger())
        && canonicalTime(receipt.value("checked_at")) && receipt.value("checked_at").toString() >= plan.value("started_at").toString()
        && receipt.value("binding") == integrityBinding(archive, revision, size, run)
        && validArchive(archive, false, request, error);
}
}

QJsonObject IncrementalRepository::checkIntegrity(const IncrementalWriteRequest &request, BackupProvider &provider, bool force)
{
    QJsonObject report{{"status", "unknown"}, {"daily_check", "Original verification evidence plus existence/size and trustworthy SHA-256 when available; size-only checks do not detect same-size corruption."},
        {"checked_objects", 0}, {"checked_bytes", 0}, {"total_objects", 0}, {"total_bytes", 0},
        {"damaged_objects", QJsonArray{}}, {"affected_snapshots", QJsonArray{}}, {"cleanup_blocked", true}, {"reuse_blocked", true}};
    Store store(request, provider);
    store.completeListings = true;
    const auto failed = [&] {
        report["status"] = store.error.contains("staging", Qt::CaseInsensitive) || store.error.contains("storage", Qt::CaseInsensitive)
            ? "waiting" : "retrying";
        report["error"] = store.error.isEmpty() ? "Integrity evidence is unreadable or inconsistent; retry without deleting history." : store.error;
        return report;
    };
    if (!store.open()) return failed();
    provider.beginBackupOperation();
    const auto end = qScopeGuard([&] { provider.endBackupOperation(); });
    if (!store.owner()) return failed();
    // Corruption knowledge is independent of cancellable/sliced coverage. An
    // immutable object ID never becomes reusable by losing its cycle cursor.
    QSet<QString> knownDamage;
    QMap<QString, QJsonObject> damageRecords;
    const auto validDamage = [&](const QJsonObject &damage, const QString &id) {
        return damage.size() == 3 && damage.value("version") == QJsonValue(1) && canonicalTime(damage.value("detected_at"))
            && damage.value("archive").toObject().value("id") == id
            && validArchive(damage.value("archive").toObject(), false, request, &store.error);
    };
    const auto damageLedger = store.lookup("integrity-damage");
    if (damageLedger == RemoteLookup::Unknown) return failed();
    if (damageLedger == RemoteLookup::Found) {
        QStringList records;
        if (!store.children("integrity-damage", &records)) return failed();
        for (const auto &name : records) {
            const QString id = name.left(name.size() - 5);
            QJsonObject damage;
            if (!IncrementalManifest::validId(id) || name != id + ".json"
                || !store.get("integrity-damage/" + name, &damage) || !validDamage(damage, id)) return failed();
            knownDamage.insert(id);
            damageRecords.insert(id, damage);
            if (!save(QDir(store.repositoryJournal).filePath("integrity-damage-" + id + ".json"), damage, &store.error)) return failed();
        }
    }
    const auto localDamage = QDir(store.repositoryJournal).entryList({"integrity-damage-*.json"}, QDir::Files | QDir::Hidden);
    for (const auto &name : localDamage) {
        const QString id = name.mid(17, name.size() - 22);
        QFile file(QDir(store.repositoryJournal).filePath(name));
        if (!IncrementalManifest::validId(id) || QFileInfo(file).isSymLink() || !file.open(QIODevice::ReadOnly)) return failed();
        const auto document = QJsonDocument::fromJson(file.readAll());
        const auto damage = document.object();
        if (file.error() != QFileDevice::NoError || !document.isObject() || !validDamage(damage, id)
            || (damageRecords.contains(id) && damageRecords.value(id).value("archive") != damage.value("archive"))) return failed();
        if (!damageRecords.contains(id)) {
            if (!store.put("integrity-damage/" + id + ".json", damage)) return failed();
            damageRecords.insert(id, damage);
        }
        knownDamage.insert(id);
    }
    report["known_damaged_objects"] = QJsonArray::fromStringList(knownDamage.values());
    report["reuse_blocked"] = false;
    QMap<QString, QJsonObject> retired;
    QString activeCleanup;
    if (!cleanupRecords(store, request, &retired, &activeCleanup)) return failed();
    if (!activeCleanup.isEmpty()) {
        report["blocked_reason"] = "pending_cleanup";
        store.error = "Finish the pending exact-scope cleanup before integrity maintenance."; return failed();
    }
    IntegrityInventory inventory;
    if (!integrityInventory(store, request, &inventory)) return failed();
    QJsonArray damaged;
    QSet<QString> affected;
    for (const auto &id : knownDamage) if (inventory.archives.contains(id)) {
        damaged.append(id);
        for (const auto &snapshot : inventory.snapshots.value(id)) affected.insert(snapshot);
    }
    report["damaged_objects"] = damaged;
    report["affected_snapshots"] = QJsonArray::fromStringList(affected.values());
    qint64 totalBytes = 0;
    for (const auto &archive : inventory.archives) totalBytes += archive.value("size").toInteger();
    report["total_objects"] = inventory.archives.size();
    report["total_bytes"] = totalBytes;
    const auto ledger = store.lookup("integrity");
    if (ledger == RemoteLookup::Unknown) return failed();
    QStringList cycles;
    if (ledger == RemoteLookup::Found && !store.children("integrity", &cycles)) return failed();
    report["ledger_cycles"] = QJsonArray::fromStringList(cycles);
    QString active;
    QJsonObject plan;
    QDateTime lastComplete;
    QDateTime lastCancelled;
    QJsonObject lastCoverage;
    for (const auto &id : cycles) {
        if (!IncrementalManifest::validId(id)) { store.error = "Unknown integrity ledger entry."; return failed(); }
        const QString root = "integrity/" + id;
        const auto published = store.lookup(root + "/plan.json");
        if (published == RemoteLookup::Unknown) return failed();
        if (published == RemoteLookup::Missing) {
            QStringList files;
            if (!store.children(root, &files)) return failed();
            if (files.isEmpty()) continue;
            store.error = "Unpublished integrity ledger contains unknown evidence."; return failed();
        }
        QJsonObject candidate;
        if (!store.get(root + "/plan.json", &candidate)) return failed();
        auto previous = request;
        const auto owner = candidate.value("anchor").toObject();
        previous.computerId = owner.value("owner_computer_id").toString();
        previous.ownershipEpoch = owner.value("ownership_epoch").toInteger(0);
        if (candidate.size() != 5 || candidate.value("version") != QJsonValue(1) || candidate.value("id") != id
            || !canonicalTime(candidate.value("started_at")) || !candidate.value("objects").isArray()
            || !IncrementalManifest::validId(previous.computerId) || previous.ownershipEpoch < 1
            || previous.ownershipEpoch > request.ownershipEpoch || owner != anchor(previous)) return failed();
        QString previousId;
        for (const auto &value : candidate.value("objects").toArray()) {
            const auto archive = value.toObject();
            const QString objectId = archive.value("id").toString();
            if (objectId <= previousId || !validArchive(archive, false, request, &store.error)) return failed();
            previousId = objectId;
        }
        const auto done = store.lookup(root + "/complete.json");
        const auto cancelled = store.lookup(root + "/cancelled.json");
        if (done == RemoteLookup::Unknown || cancelled == RemoteLookup::Unknown) return failed();
        if (done == RemoteLookup::Found && cancelled == RemoteLookup::Found) return failed();
        if (cancelled == RemoteLookup::Found) {
            QJsonObject marker;
            if (!store.get(root + "/cancelled.json", &marker) || marker.size() != 2
                || marker.value("plan") != digest(json(candidate)) || !canonicalTime(marker.value("cancelled_at"))
                || marker.value("cancelled_at").toString() < candidate.value("started_at").toString()) return failed();
            const auto time = QDateTime::fromString(marker.value("cancelled_at").toString(), Qt::ISODateWithMs);
            if (!lastCancelled.isValid() || time > lastCancelled) lastCancelled = time;
            continue;
        }
        if (done == RemoteLookup::Found) {
            QJsonObject completion;
            if (!store.get(root + "/complete.json", &completion) || completion.size() != 6
                || completion.value("plan") != digest(json(candidate)) || !canonicalTime(completion.value("completed_at"))
                || completion.value("completed_at").toString() < candidate.value("started_at").toString()
                || !completion.value("objects").isArray() || !completion.value("unreachable_objects").isArray()
                || completion.value("checked_objects").toInteger(-1) != completion.value("objects").toArray().size()
                || completion.value("checked_bytes").toInteger(-1) < 0) return failed();
            qint64 bytes = 0;
            QString previousObject;
            QMap<QString, QJsonObject> planned;
            QSet<QString> reconciled;
            for (const auto &value : candidate.value("objects").toArray()) planned.insert(value.toObject().value("id").toString(), value.toObject());
            for (const auto &value : completion.value("objects").toArray()) {
                const auto receipt = value.toObject();
                const auto archive = receipt.value("archive").toObject();
                const QString object = archive.value("id").toString();
                if (!validIntegrityReceipt(receipt, candidate, request, &store.error) || receipt.value("state") != "good"
                    || receipt.value("checked_at").toString() > completion.value("completed_at").toString()
                    || object <= previousObject || !validArchive(archive, false, request, &store.error)) return failed();
                previousObject = object;
                if (planned.contains(object)) {
                    if (planned.value(object) != archive) return failed();
                    reconciled.insert(object);
                }
                bytes += archive.value("size").toInteger();
            }
            previousObject.clear();
            for (const auto &value : completion.value("unreachable_objects").toArray()) {
                const auto archive = value.toObject();
                const QString object = archive.value("id").toString();
                if (object <= previousObject || !planned.contains(object) || planned.value(object) != archive
                    || reconciled.contains(object) || inventory.archives.contains(object)) return failed();
                previousObject = object;
                reconciled.insert(object);
            }
            if (reconciled.size() != planned.size()) return failed();
            if (completion.value("checked_bytes").toInteger(-1) != bytes) return failed();
            const auto time = QDateTime::fromString(completion.value("completed_at").toString(), Qt::ISODateWithMs);
            if (!lastComplete.isValid() || time > lastComplete) { lastComplete = time; lastCoverage = completion; }
        } else {
            if (!active.isEmpty()) { store.error = "Multiple unfinished integrity cycles require reconciliation."; return failed(); }
            active = id;
            plan = candidate;
        }
    }
    report["last_completed_at"] = lastComplete.toString(Qt::ISODateWithMs);
    report["next_due_at"] = lastComplete.isValid() ? lastComplete.addDays(30).toString(Qt::ISODateWithMs) : QString();
    const bool due = !lastComplete.isValid() || request.scan.checkedAt >= lastComplete.addDays(30)
        || (lastCancelled.isValid() && lastCancelled >= lastComplete);
    report["overdue"] = due;
    if (active.isEmpty() && !force && !due && damaged.isEmpty()) {
        report["status"] = "not_due";
        report["cleanup_blocked"] = false;
        report["last_coverage"] = lastCoverage;
        return report;
    }
    if (active.isEmpty()) {
        active = uuid();
        plan = {{"version", 1}, {"id", active}, {"anchor", anchor(request)},
            {"started_at", request.scan.checkedAt.toUTC().toString(Qt::ISODateWithMs)}, {"objects", integrityWorklist(inventory)}};
        if (!store.put("integrity/" + active + "/plan.json", plan)) return failed();
        cycles.append(active);
        cycles.sort();
        report["ledger_cycles"] = QJsonArray::fromStringList(cycles);
    }
    if (plan.value("anchor") != anchor(request)) { store.error = "Unfinished integrity cycle belongs to an old owner/epoch; do not resume a stale continuation."; return failed(); }
    report["cycle_id"] = active;
    report["started_at"] = plan.value("started_at");
    report["status"] = "in_progress";
    const QString root = "integrity/" + active;
    const auto initialControl = request.checkpoint ? request.checkpoint("integrity-started") : IncrementalControl::Continue;
    if (initialControl != IncrementalControl::Continue || (request.scan.stopped && request.scan.stopped())) {
        report["status"] = initialControl == IncrementalControl::Cancel ? "cancelled" : "paused";
        if (initialControl == IncrementalControl::Cancel && !store.put(root + "/cancelled.json", QJsonObject{
            {"plan", digest(json(plan))}, {"cancelled_at", request.scan.checkedAt.toUTC().toString(Qt::ISODateWithMs)}})) return failed();
        return report;
    }
    const QString run = uuid();
    QElapsedTimer timer;
    timer.start();
    qint64 downloaded = 0, checkedBytes = 0;
    int checked = 0;
    QJsonArray evidence;
    bool stopped = false;
    QJsonArray oversized;
    for (auto it = inventory.archives.cbegin(); it != inventory.archives.cend(); ++it) {
        const auto archive = it.value();
        if (archive.value("size").toInteger() > request.stagingBudget && archive.value("members").toArray().size() == 1) {
            oversized.append(it.key());
            report["oversized_single_file_objects"] = oversized;
        }
        const QString relative = archive.value("remote").toString();
        const auto found = store.lookup(relative);
        if (found == RemoteLookup::Unknown) return failed();
        RemoteFile metadata;
        if (found == RemoteLookup::Found && (!provider.inspect(store.remote(relative), &metadata, &store.error)
            || metadata.path != store.remote(relative) || metadata.size < 0)) return failed();
        // A trustworthy content hash is itself a content revision. An unknown
        // revision can only reuse evidence within this invocation, never after a crash.
        const QString revision = provider.trustworthyContentChecksums() && !metadata.checksum.isEmpty()
            ? "sha256:" + QString::fromLatin1(metadata.checksum.toHex()) : metadata.revision;
        if (revision.isEmpty()) report["resume_warning"] = "Provider supplied no content revision or trustworthy checksum. Previously checked objects must be downloaded again after interruption; increase the slice budget if needed to complete safely in one invocation.";
        const QString binding = integrityBinding(archive, revision, metadata.size, revision.isEmpty() ? run : QString());
        const QString receiptPath = root + "/checks/" + it.key() + '/' + binding + ".json";
        if (!store.owner() || !provider.ensureDirectory(QFileInfo(store.remote(receiptPath)).path(), &store.error)) return failed();
        const auto receiptFound = store.lookup(receiptPath);
        if (receiptFound == RemoteLookup::Unknown) return failed();
        QJsonObject receipt;
        if (receiptFound == RemoteLookup::Found) {
            if (!store.get(receiptPath, &receipt) || !validIntegrityReceipt(receipt, plan, request, &store.error) || receipt.value("binding") != binding
                || receipt.value("archive") != archive || receipt.value("revision") != revision
                || receipt.value("observed_size") != QJsonValue(metadata.size) || receipt.value("run") != (revision.isEmpty() ? run : QString())
                || !canonicalTime(receipt.value("checked_at")) || receipt.value("checked_at").toString() < plan.value("started_at").toString()
                || receipt.value("method") != "download-sha256-inventory"
                || (receipt.value("state") != "good" && receipt.value("state") != "damaged") || receipt.value("cycle") != active) return failed();
        } else {
            if (downloaded >= request.integrityByteBudget || timer.elapsed() >= request.integrityTimeBudgetMs) { stopped = true; break; }
            const auto control = request.checkpoint ? request.checkpoint("integrity-object") : IncrementalControl::Continue;
            if (control != IncrementalControl::Continue || (request.scan.stopped && request.scan.stopped())) {
                report["status"] = control == IncrementalControl::Cancel ? "cancelled" : "paused";
                if (control == IncrementalControl::Cancel && !store.put(root + "/cancelled.json", QJsonObject{
                    {"plan", digest(json(plan))}, {"cancelled_at", request.scan.checkedAt.toUTC().toString(Qt::ISODateWithMs)}})) return failed();
                stopped = true; break;
            }
            const auto state = health(store, provider, archive, true);
            if (state == Health::Unknown) return failed();
            if (found == RemoteLookup::Found && metadata.size == archive.value("size").toInteger()
                && (!provider.trustworthyContentChecksums() || metadata.checksum.isEmpty()
                    || metadata.checksum == QByteArray::fromHex(archive.value("sha256").toString().toLatin1()))) downloaded += metadata.size;
            RemoteFile after;
            if (found == RemoteLookup::Found && (!provider.inspect(store.remote(relative), &after, &store.error)
                || after.path != metadata.path || after.size != metadata.size || after.revision != metadata.revision || after.checksum != metadata.checksum)) {
                store.error = "Remote revision changed during integrity verification; retry."; return failed();
            }
            receipt = {{"binding", binding}, {"archive", archive}, {"revision", revision}, {"cycle", active},
                {"observed_size", metadata.size}, {"run", revision.isEmpty() ? run : QString()},
                {"checked_at", request.scan.checkedAt.toUTC().toString(Qt::ISODateWithMs)},
                {"method", "download-sha256-inventory"}, {"state", state == Health::Good ? "good" : "damaged"}};
        }
        if (receipt.value("state") == "damaged" || knownDamage.contains(it.key())) {
            if (!damaged.contains(it.key())) damaged.append(it.key());
            for (const auto &snapshot : inventory.snapshots.value(it.key())) affected.insert(snapshot);
            report["damaged_objects"] = damaged;
            report["affected_snapshots"] = QJsonArray::fromStringList(affected.values());
            if (!knownDamage.contains(it.key())) {
                knownDamage.insert(it.key());
                report["known_damaged_objects"] = QJsonArray::fromStringList(knownDamage.values());
                if (!persistDamage(store, archive, request.scan.checkedAt)) return failed();
            }
        } else { ++checked; checkedBytes += archive.value("size").toInteger(); }
        if (!store.put(receiptPath, receipt) || !store.local("integrity-cursor.json", QJsonObject{{"cycle", active}, {"receipt", receiptPath}})) return failed();
        evidence.append(receipt);
        report["checked_objects"] = checked;
        report["checked_bytes"] = checkedBytes;
        report["damaged_objects"] = damaged;
        report["affected_snapshots"] = QJsonArray::fromStringList(affected.values());
    }
    report["checked_objects"] = checked;
    report["checked_bytes"] = checkedBytes;
    report["remaining_objects"] = inventory.archives.size() - checked;
    report["downloaded_bytes"] = downloaded;
    report["damaged_objects"] = damaged;
    QStringList affectedList = affected.values();
    affectedList.sort();
    report["affected_snapshots"] = QJsonArray::fromStringList(affectedList);
    if (!damaged.isEmpty()) report["status"] = "damaged";
    if (!stopped && damaged.isEmpty()) {
        IntegrityInventory fresh;
        if (!store.owner() || !integrityInventory(store, request, &fresh)) return failed();
        if (integrityWorklist(fresh) != integrityWorklist(inventory)) { store.error = "Reachability changed; resume to reconcile coverage."; return failed(); }
        for (const auto &value : evidence) {
            const auto receipt = value.toObject();
            if (receipt.value("revision").toString().isEmpty()) continue; // Downloaded during this invocation.
            RemoteFile current;
            if (!provider.inspect(store.remote(receipt.value("archive").toObject().value("remote").toString()), &current, &store.error)) return failed();
            const QString revision = provider.trustworthyContentChecksums() && !current.checksum.isEmpty()
                ? "sha256:" + QString::fromLatin1(current.checksum.toHex()) : current.revision;
            if (revision != receipt.value("revision").toString()
                || current.size != receipt.value("archive").toObject().value("size").toInteger()) {
                store.error = "Covered remote revision changed before completion; resume to recheck."; return failed();
            }
        }
        QJsonArray unreachable;
        for (const auto &value : plan.value("objects").toArray())
            if (!inventory.archives.contains(value.toObject().value("id").toString())) unreachable.append(value);
        const QJsonObject completion{{"plan", digest(json(plan))}, {"completed_at", request.scan.checkedAt.toUTC().toString(Qt::ISODateWithMs)},
            {"objects", evidence}, {"unreachable_objects", unreachable}, {"checked_objects", checked}, {"checked_bytes", checkedBytes}};
        if (!store.put(root + "/complete.json", completion) || !store.local("integrity-complete.json", completion)) return failed();
        report["status"] = "complete";
        report["last_completed_at"] = completion.value("completed_at");
        report["next_due_at"] = request.scan.checkedAt.addDays(30).toUTC().toString(Qt::ISODateWithMs);
        report["overdue"] = false;
        report["cleanup_blocked"] = false;
    }
    return report;
}

static IncrementalWriteResult executeWrite(const IncrementalWriteRequest &request, BackupProvider &provider,
    const QSet<QString> &knownDamage)
{
    IncrementalWriteResult result;
    result.lastSuccessfulCheck = request.scan.lastSuccessfulCheck;
    Store store(request, provider);
    const auto checkArchive = [&](const QJsonObject &archive, bool full) {
        QElapsedTimer timer; timer.start();
        const auto result = knownDamage.contains(archive.value("id").toString()) ? Health::Damaged : health(store, provider, archive, full);
        if (request.phaseMeasured) request.phaseMeasured(full ? "full-object-verification" : "daily-reuse-check", timer.nsecsElapsed());
        return result;
    };
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
    QMap<QString, QJsonObject> retired;
    QString activeCleanup;
    if (!cleanupRecords(store, request, &retired, &activeCleanup)) return fail("cleanup-recovery");
    if (!activeCleanup.isEmpty()) {
        store.error = "Finish or reconcile the pending cleanup before resuming any writer.";
        return fail("cleanup-recovery");
    }
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
    auto completionHistory = retired;
    for (auto it = history.cbegin(); it != history.cend(); ++it) completionHistory.insert(it.key(), it.value());
    for (const auto &completed : previousCompletions)
        if (!completionMatchesHistory(completed, completionHistory, &store.error)) return fail("operation-discovery");
    if (!ownCompletion.isEmpty() && !completionMatchesHistory(ownCompletion, completionHistory, &store.error)) return fail("success-recovery");
    if (!ownCompletion.isEmpty() && retired.contains(ownCompletion.value("snapshot_id").toString())) {
        result.state = ownCompletion.value("state") == "saved" ? IncrementalWriteResult::State::Saved : IncrementalWriteResult::State::Unchanged;
        result.lastSavedChange = QDateTime::fromString(ownCompletion.value("last_saved_change").toString(), Qt::ISODateWithMs);
        result.lastSuccessfulCheck = QDateTime::fromString(ownCompletion.value("checked_at").toString(), Qt::ISODateWithMs);
        return result; // Never republish an expired snapshot by replaying its old operation.
    }
    result.lastSavedChange = latestTime;
    if (!latestId.isEmpty()) result.latestSnapshotPath = store.remote("snapshots/" + latestId);
    const auto noteDamage = [&](const QJsonObject &archive) {
        const QString id = archive.value("id").toString();
        if (!result.damagedObjects.contains(id)) result.damagedObjects.append(id);
        for (auto snapshot = history.cbegin(); snapshot != history.cend(); ++snapshot)
            for (const auto &value : snapshot.value().value("archives").toArray())
                if (value.toObject().value("id").toString() == id && !result.affectedSnapshots.contains(snapshot.key()))
                    result.affectedSnapshots.append(snapshot.key());
        result.damagedObjects.sort();
        result.affectedSnapshots.sort();
        return persistDamage(store, archive, request.scan.checkedAt);
    };
    const auto finish = [&](const QJsonObject &manifest, bool unchanged) {
        result.latestSnapshotPath = store.remote("snapshots/" + (unchanged ? latestId : request.snapshotId));
        const auto savedState = unchanged ? history.value(latestId) : manifest;
        result.savedFiles = savedState.value("entries").toArray().size();
        for (const auto &entry : savedState.value("entries").toArray()) result.savedBytes += entry.toObject().value("size").toInteger();
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
            const auto state = checkArchive(value.toObject(), false);
            if (state == Health::Unknown) return fail("publication-recovery");
            if (state == Health::Damaged) { if (!noteDamage(value.toObject())) return fail("damage-checkpoint"); store.error = "Publication references damaged data; start a new operation, never rewrite its immutable intent."; return fail("publication-recovery", true); }
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
    QElapsedTimer planningTimer; planningTimer.start();
    const auto plan = IncrementalPlanner::plan(scan);
    if (request.phaseMeasured) request.phaseMeasured("local-plan-hash", planningTimer.nsecsElapsed());
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
        const auto state = checkArchive(archive, !verified);
        if (state == Health::Unknown) return fail("continuation-verification");
        if (state == Health::Damaged) { if (!noteDamage(archive)) return fail("damage-checkpoint"); continue; }
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
        const auto state = checkArchive(item.value(), false);
        if (state == Health::Unknown) return fail("reuse-check");
        if (state == Health::Damaged) {
            repairRequired = repairRequired || originallyRequired.contains(item.key());
            if (!noteDamage(item.value())) return fail("damage-checkpoint");
            item = available.erase(item);
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
        QElapsedTimer captureTimer; captureTimer.start();
        for (const auto &file : group) {
            const QString member = "file-" + QString::number(index++);
            if (!IncrementalPlanner::stage(file, QDir(tree).filePath(member), &store.error, stopped)) {
                if (activeControl != IncrementalControl::Continue) { applyControl(activeControl); return result; }
                return fail("source-capture", !store.error.startsWith("Waiting"));
            }
            entries.append({file.sourcePath, "payload/" + member, file.size, file.checksum, file.restorePath});
        }
        if (request.phaseMeasured) request.phaseMeasured("private-source-capture", captureTimer.nsecsElapsed());
        if (!checkpoint("prepared-sources")) return result;
        const QString id = uuid();
        const QString archivePath = QDir(payloads).filePath(id + ".tar.gz");
        QElapsedTimer compressionTimer; compressionTimer.start();
        const bool packed = BackupArchiveIO::pack(tree, "payload", archivePath, archiveAllowance, &entries, &store.error, stopped);
        if (request.phaseMeasured) request.phaseMeasured("compression", compressionTimer.nsecsElapsed());
        if (!packed) {
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
        if (verified == Health::Damaged) { if (!noteDamage(archive)) return fail("damage-checkpoint"); store.error = "New upload failed full verification; immutable data will not be overwritten."; return fail("verification"); }
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
        const auto state = checkArchive(value.toObject(), false);
        if (state == Health::Unknown) return fail("final-reuse-check");
        if (state == Health::Damaged) { if (!noteDamage(value.toObject())) return fail("damage-checkpoint"); store.error = "Remote object changed before publication; retry with replacement content."; return fail("final-reuse-check"); }
    }
    publication = manifestBase(request, QDateTime::fromString(identity.value("created_at").toString(), Qt::ISODateWithMs));
    publication["archives"] = archives;
    publication["entries"] = entries;
    if (!IncrementalManifest::validate(json(publication), request.scan.setId, request.snapshotId, &store.error)) return fail("manifest-validation", true);
    if (!store.local("publication.json", publication) || !store.put(operation + "/publication.json", publication)) return fail("publication-intent");
    if (!checkpoint("publication-prepared")) return result;
    QElapsedTimer publicationTimer; publicationTimer.start();
    const bool published = store.put(snapshot, publication);
    if (request.phaseMeasured) request.phaseMeasured("manifest-publication", publicationTimer.nsecsElapsed());
    if (!published) return fail("publication");
    result.manifestCommitted = true;
    if (!checkpoint("published")) return result;
    return finish(publication, false);
}

namespace {
struct CleanupInventory {
    QMap<QString, QJsonObject> snapshots;
    QMap<QString, QJsonObject> archives;
    QSet<QString> liveMembers;
    QSet<QString> liveObjects;
    QJsonArray legacy;
};

bool externalJson(Store &store, BackupProvider &provider, const QString &path, QJsonObject *object)
{
    QTemporaryDir directory(QDir(store.staging.path()).filePath("legacy-XXXXXX"));
    const QString local = directory.filePath("manifest.json");
    if (!directory.isValid() || !provider.download(path, local, &store.error)) return false;
    QFile file(local);
    if (!file.open(QIODevice::ReadOnly)) { store.error = "Unreadable legacy manifest."; return false; }
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (file.error() != QFileDevice::NoError || !document.isObject()) { store.error = "Malformed legacy manifest."; return false; }
    *object = document.object();
    return true;
}

bool cleanupInventory(Store &store, BackupProvider &provider, const IncrementalWriteRequest &request,
    CleanupInventory *inventory)
{
    QStringList snapshots;
    if (!store.owner() || !store.children("snapshots", &snapshots)) return false;
    for (const auto &id : snapshots) {
        if (!IncrementalManifest::validId(id)) { store.error = "Unknown snapshot blocks cleanup."; return false; }
        const QString path = "snapshots/" + id + "/manifest.json";
        const auto found = store.lookup(path);
        if (found == RemoteLookup::Unknown) return false;
        if (found == RemoteLookup::Missing) continue;
        QJsonObject manifest;
        if (!store.get(path, &manifest) || !IncrementalManifest::validate(json(manifest), request.scan.setId, id, &store.error)
            || manifest.value("origin_computer_id") != request.originComputerId) return false;
        inventory->snapshots.insert(id, manifest);
        for (const auto &value : manifest.value("archives").toArray()) {
            const auto archive = value.toObject();
            const QString object = archive.value("id").toString();
            if (inventory->archives.contains(object) && inventory->archives.value(object) != archive) {
                store.error = "Conflicting archive descriptors block cleanup."; return false;
            }
            inventory->archives.insert(object, archive);
            inventory->liveObjects.insert(object);
            if (health(store, provider, archive, false) != Health::Good) {
                store.error = "Unknown/damaged saved object blocks cleanup."; return false;
            }
        }
        for (const auto &value : manifest.value("entries").toArray()) {
            const auto entry = value.toObject();
            inventory->liveMembers.insert(entry.value("archive").toString() + '/' + entry.value("member").toString());
        }
    }
    return true;
}

// An unresolved writer conservatively blocks ALL cleanup. This is stronger than
// object-by-object protection and also covers incomplete/unknown protection files.
bool terminalOperations(Store &store, BackupProvider &provider, const IncrementalWriteRequest &request,
    const QMap<QString, QJsonObject> &history, bool *successfulPublication,
    QMap<QString, QJsonObject> *archives = nullptr)
{
    QMap<QString, QJsonObject> descriptors, preparations;
    const auto descriptor = [&](const QJsonObject &archive) {
        const QString id = archive.value("id").toString();
        if (descriptors.contains(id) && descriptors.value(id) != archive) {
            store.error = "Conflicting protection descriptors."; return false;
        }
        descriptors.insert(id, archive);
        return true;
    };
    QStringList operations;
    if (!store.children("operations", &operations)) return false;
    for (const auto &id : operations) {
        if (!IncrementalManifest::validId(id)) { store.error = "Unknown operation blocks cleanup."; return false; }
        const QString root = "operations/" + id;
        QJsonObject identity, terminal;
        if (!store.get(root + "/identity.json", &identity) || !validPriorIdentity(identity, id, request)) {
            store.error = "Unreadable or invalid operation identity blocks cleanup."; return false;
        }
        const auto complete = store.lookup(root + "/complete.json");
        const auto cancelled = store.lookup(root + "/cancelled.json");
        if (complete == RemoteLookup::Unknown || cancelled == RemoteLookup::Unknown) return false;
        if ((complete == RemoteLookup::Found) == (cancelled == RemoteLookup::Found)) {
            store.error = "Protected/resumable or conflicting operation blocks cleanup: " + id; return false;
        }
        if (complete == RemoteLookup::Found) {
            if (!store.get(root + "/complete.json", &terminal)
                || !validCompletion(terminal, id, identity.value("snapshot_id").toString(), &store.error)
                || !completionMatchesHistory(terminal, history, &store.error)) return false;
            if (id == request.operationId && terminal.value("state") == "saved"
                && terminal.value("snapshot_id") == request.snapshotId) *successfulPublication = true;
        } else if (!store.get(root + "/cancelled.json", &terminal)
            || terminal != QJsonObject{{"version", 1}, {"operation_id", id}, {"state", "cancelled"}}) {
            store.error = "Invalid cancellation record."; return false;
        }
        QStringList records;
        if (!store.children(root, &records)) return false;
        for (const auto &name : records) {
            QJsonObject record;
            if (!store.get(root + '/' + name, &record)) return false;
            if (name == "identity.json" || name == "complete.json" || name == "cancelled.json") continue;
            if (name == "publication.json") {
                if (!IncrementalManifest::validate(json(record), request.scan.setId, identity.value("snapshot_id").toString(), &store.error)) return false;
                for (const auto &value : record.value("archives").toArray()) if (!descriptor(value.toObject())) return false;
                continue;
            }
            const QString prefix = name.startsWith("prepared-") ? "prepared-" : name.startsWith("verified-") ? "verified-" : name.startsWith("reuse-") ? "reuse-" : QString();
            const QString objectId = name.mid(prefix.size(), name.size() - prefix.size() - 5);
            if (prefix.isEmpty() || !IncrementalManifest::validId(objectId) || name != prefix + objectId + ".json"
                || record.size() != 2 || record.value("version") != QJsonValue(1)) {
                store.error = "Unknown protection record blocks cleanup."; return false;
            }
            if (prefix == "reuse-") {
                if (!record.value("archives").isArray()) return false;
                for (const auto &archive : record.value("archives").toArray())
                    if (!validArchive(archive.toObject(), true, request, &store.error) || !descriptor(archive.toObject())) return false;
            } else if (!validArchive(record.value("archive").toObject(), prefix == "verified-", request, &store.error)
                || record.value("archive").toObject().value("id") != objectId) return false;
            else if (prefix == "verified-") {
                if (!descriptor(record.value("archive").toObject())) return false;
            } else {
                const auto archive = record.value("archive").toObject();
                if (preparations.contains(objectId) && preparations.value(objectId) != archive) {
                    store.error = "Conflicting prepared object descriptors."; return false;
                }
                preparations.insert(objectId, archive);
            }
        }
    }
    for (auto it = preparations.cbegin(); it != preparations.cend(); ++it) {
        if (descriptors.contains(it.key())) {
            auto physical = descriptors.value(it.key());
            physical.remove("original_verification");
            if (physical != it.value()) { store.error = "Prepared/verified identity conflict."; return false; }
        } else if (archives) {
            const auto found = store.lookup(it.value().value("remote").toString());
            if (found == RemoteLookup::Unknown) return false;
            if (found == RemoteLookup::Missing) continue;
            auto archive = it.value();
            if (health(store, provider, archive, true) != Health::Good) return false;
            archive["original_verification"] = QJsonObject{{"method", "download-sha256"},
                {"verified_at", request.scan.checkedAt.toUTC().toString(Qt::ISODateWithMs)}, {"sha256", archive.value("sha256")}};
            descriptors.insert(it.key(), archive);
        }
    }
    if (archives) for (auto it = descriptors.cbegin(); it != descriptors.cend(); ++it) {
        if (archives->contains(it.key()) && archives->value(it.key()) != it.value()) {
            store.error = "Conflicting saved/protection object descriptors."; return false;
        }
        archives->insert(it.key(), it.value());
    }
    return true;
}

bool legacyInventory(Store &store, BackupProvider &provider, const IncrementalWriteRequest &request, QJsonArray *copies)
{
    for (const auto &root : legacyScopes(request)) {
        // Validate containment even when there are no deletion candidates.
        if (root != QDir::cleanPath(root) || root == "/" || root == "." || root.contains('\\')
            || root.contains(QChar::Null) || root.split('/').contains("..") || root == request.remoteSetRoot
            || root.startsWith(request.remoteSetRoot + '/') || request.remoteSetRoot.startsWith(root + '/')) {
            store.error = "Unsafe legacy root."; return false;
        }
        QVector<RemoteItem> items;
        if (!provider.listCompleteIfPresent(root, &items, &store.error)) return false;
        QSet<QString> seen;
        for (const auto &item : items) {
            if (item.name.isEmpty() || item.name == "." || item.name == ".." || item.name.contains('/')
                || item.name.contains('\\') || item.name.contains(QChar::Null) || item.path != root + '/' + item.name
                || seen.contains(item.name) || !item.directory) {
                store.error = "Ambiguous legacy inventory blocks cleanup."; return false;
            }
            seen.insert(item.name);
            RemoteCopy copy;
            QJsonObject manifest;
            if (!BackupCatalog::verifyCopy(provider, item.path, request.legacySetId.isEmpty() ? request.scan.setId : request.legacySetId, &copy, &store.error)
                || !externalJson(store, provider, item.path + "/manifest.json", &manifest)) return false;
            if (copy.complete()) copies->append(QJsonObject{{"path", item.path}, {"manifest", manifest}});
        }
    }
    return true;
}
} // namespace

bool IncrementalRepository::cleanup(const IncrementalWriteRequest &request, BackupProvider &provider,
    QJsonObject *presented, QString *confirmation, QString *error)
{
    QString reason;
    if (!error) error = &reason;
    // Advance due maintenance before acquiring the deletion lock. A previously
    // published deletion transaction is reconciled below, never broadened.
    const auto integrity = checkIntegrity(request, provider);
    if (integrity.value("cleanup_blocked").toBool(true)
        && integrity.value("blocked_reason") != "pending_cleanup") {
        *error = "Integrity maintenance blocks cleanup: " + integrity.value("status").toString() + ". " + integrity.value("error").toString();
        return false;
    }
    Store store(request, provider);
    store.completeListings = true;
    const auto failed = [&] { *error = store.error.isEmpty() ? "Cleanup validation failed; nothing further was deleted." : store.error; return false; };
    if (!store.open()) return failed();
    provider.beginBackupOperation();
    const auto end = qScopeGuard([&] { provider.endBackupOperation(); });
    if (!store.owner()) return failed();
    QMap<QString, QJsonObject> retired;
    QString active;
    if (!cleanupRecords(store, request, &retired, &active)) return failed();
    if (active.isEmpty()) {
        QStringList cycles;
        const auto ledger = store.lookup("integrity");
        if (ledger == RemoteLookup::Unknown || (ledger == RemoteLookup::Found && !store.children("integrity", &cycles))) return failed();
        if (QJsonArray::fromStringList(cycles) != integrity.value("ledger_cycles").toArray()) {
            store.error = "Integrity ledger changed before acquiring the cleanup lock; retry maintenance first."; return failed();
        }
    }
    CleanupInventory inventory;
    if (!cleanupInventory(store, provider, request, &inventory)) return failed();
    auto history = retired;
    for (auto it = inventory.snapshots.cbegin(); it != inventory.snapshots.cend(); ++it) history.insert(it.key(), it.value());
    QJsonObject proposal;
    if (!active.isEmpty()) {
        if (!store.get("maintenance/" + active + "/proposal.json", &proposal)) return failed();
        for (const auto &value : proposal.value("snapshots").toArray()) {
            const auto manifest = value.toObject();
            history.insert(manifest.value("snapshot_id").toString(), manifest);
        }
    }
    bool saved = false;
    if (!terminalOperations(store, provider, request, history, &saved, active.isEmpty() ? &inventory.archives : nullptr)) return failed();
    if (active.isEmpty()) {
        if (request.retention < 1 || !saved || !inventory.snapshots.contains(request.snapshotId)) {
            *error = "Cleanup requires a durably successful saved publication and positive retention."; return false;
        }
        if (!legacyInventory(store, provider, request, &inventory.legacy)) return failed();
        QStringList objectsPresent;
        if (!store.children("objects", &objectsPresent)) return failed();
        QSet<QString> present;
        for (const auto &name : objectsPresent) {
            const QString id = name.left(name.size() - 7);
            if (!IncrementalManifest::validId(id) || name != id + ".tar.gz" || !inventory.archives.contains(id)) {
                store.error = "Unknown object or missing protection metadata blocks cleanup."; return failed();
            }
            present.insert(id);
        }
        for (auto it = inventory.archives.begin(); it != inventory.archives.end();)
            if (!present.contains(it.key())) it = inventory.archives.erase(it);
            else ++it;
        struct SavedState { QString identity; QString time; QJsonObject snapshot; QJsonObject legacy; };
        QVector<SavedState> states;
        for (auto it = inventory.snapshots.cbegin(); it != inventory.snapshots.cend(); ++it)
            states.append({it.key(), it.value().value("created_at").toString(), it.value(), {}});
        for (const auto &value : inventory.legacy) {
            const auto copy = value.toObject();
            // Catalog has already validated creation time and legacy identity.
            RemoteCopy verified;
            if (!BackupCatalog::verifyCopy(provider, copy.value("path").toString(), request.legacySetId.isEmpty() ? request.scan.setId : request.legacySetId, &verified, &store.error)) return failed();
            states.append({verified.copyId, verified.createdAt.toUTC().toString(Qt::ISODateWithMs), {}, copy});
        }
        std::sort(states.begin(), states.end(), [](const SavedState &a, const SavedState &b) {
            return a.time == b.time ? a.identity > b.identity : a.time > b.time;
        });
        QJsonArray expired, legacy;
        QSet<QString> retainedObjects;
        QSet<QString> retainedMembers;
        for (qsizetype i = 0; i < states.size(); ++i) {
            if (i >= request.retention) {
                if (!states[i].snapshot.isEmpty()) expired.append(states[i].snapshot);
                else legacy.append(states[i].legacy);
            } else {
                for (const auto &value : states[i].snapshot.value("archives").toArray())
                    retainedObjects.insert(value.toObject().value("id").toString());
                for (const auto &value : states[i].snapshot.value("entries").toArray()) {
                    const auto entry = value.toObject();
                    retainedMembers.insert(entry.value("archive").toString() + '/' + entry.value("member").toString());
                }
            }
        }
        QJsonArray objects;
        for (auto it = inventory.archives.cbegin(); it != inventory.archives.cend(); ++it) {
            if (retainedObjects.contains(it.key())) continue;
            const auto found = store.lookup(it.value().value("remote").toString());
            if (found == RemoteLookup::Unknown) return failed();
            if (found == RemoteLookup::Found) objects.append(it.value());
        }
        if (expired.isEmpty() && legacy.isEmpty() && objects.isEmpty()) { error->clear(); return true; }
        QJsonObject generation;
        for (auto it = inventory.snapshots.cbegin(); it != inventory.snapshots.cend(); ++it) generation[it.key()] = digest(json(it.value()));
        qint64 liveBytes = 0, obsoleteBytes = 0;
        for (auto it = inventory.archives.cbegin(); it != inventory.archives.cend(); ++it)
            for (const auto &value : it.value().value("members").toArray()) {
                const auto member = value.toObject();
                if (retainedMembers.contains(it.key() + '/' + member.value("path").toString())) liveBytes += member.value("size").toInteger();
                else obsoleteBytes += member.value("size").toInteger();
            }
        QJsonObject identities;
        QStringList paths;
        for (const auto &value : expired) paths.append(store.remote("snapshots/" + value.toObject().value("snapshot_id").toString() + "/manifest.json"));
        for (const auto &value : objects) paths.append(store.remote(value.toObject().value("remote").toString()));
        for (const auto &value : legacy) paths.append(value.toObject().value("path").toString());
        for (const auto &path : paths) {
            QString identity;
            if (!provider.deletionIdentity(path, &identity, &store.error) || identity.isEmpty()) return failed();
            identities[path] = identity;
        }
        active = uuid();
        proposal = {{"version", 1}, {"id", active}, {"anchor", anchor(request)}, {"retention", request.retention},
            {"generation", digest(json(generation))}, {"snapshots", expired}, {"objects", objects}, {"legacy", legacy},
            {"identities", identities}, {"estimated_live_member_bytes", liveBytes}, {"estimated_obsolete_member_bytes", obsoleteBytes}};
        if (!validCleanupProposal(proposal, request, &store.error)
            || !store.local("cleanup-" + active + ".json", proposal)
            || !store.put("maintenance/" + active + "/proposal.json", proposal)) return failed();
    }
    const QString hash = digest(json(proposal));
    if (presented) *presented = proposal;
    if (confirmation) *confirmation = hash;
    const QString root = "maintenance/" + active;
    const QJsonObject receipt{{"proposal", hash}};
    const auto authorized = store.lookup(root + "/authorized.json");
    if (authorized == RemoteLookup::Unknown) return failed();
    if (!request.cleanupDismissal.isEmpty()) {
        if (request.cleanupDismissal != hash || authorized != RemoteLookup::Missing) {
            *error = "Only the exact unapproved cleanup proposal may be dismissed."; return false;
        }
        if (!store.put(root + "/dismissed.json", receipt)) return failed();
        error->clear();
        return true;
    }
    if (authorized == RemoteLookup::Missing) {
        if (request.cleanupConfirmation != hash) {
            *error = "Review the exact cleanup proposal and confirm its SHA-256 before deletion."; return false;
        }
        if (!store.local("cleanup-" + active + ".json", proposal) || !store.put(root + "/authorized.json", receipt)) return failed();
    } else {
        QJsonObject observed;
        if (!store.get(root + "/authorized.json", &observed) || observed != receipt) {
            store.error = "Cleanup authorization does not match the proposal."; return failed();
        }
    }
    const auto checkpoint = [&](const QString &phase) {
        if (request.checkpoint && request.checkpoint(phase) != IncrementalControl::Continue) {
            store.error = "Cleanup interrupted; retry the exact persisted proposal."; return false;
        }
        return true;
    };
    if (!checkpoint("cleanup-authorized")) return failed();
    const auto remove = [&](const QString &path, const QString &key, const std::function<bool()> &validate) {
        for (const auto &phase : {QString("trashed"), QString("deleted")}) {
            const QString marker = root + '/' + key + '-' + phase + ".json";
            const auto found = store.lookup(marker);
            if (found == RemoteLookup::Unknown) return false;
            if (found == RemoteLookup::Found) {
                QJsonObject observed;
                if (!store.get(marker, &observed) || observed != receipt) { store.error = "Invalid deletion phase receipt."; return false; }
                continue;
            }
            if (!validate() || !store.owner() || !checkpoint("cleanup-before-" + key.section('-', 0, 0) + '-' + phase)) return false;
            // Repeat only the same exact path after an uncertain response. A
            // provider failure is never interpreted as absence by error text.
            const QString identity = proposal.value("identities").toObject().value(path).toString();
            if (!(phase == "trashed" ? provider.trashExact(path, identity, &store.error) : provider.deleteExact(path, identity, &store.error))) return false;
            if (!checkpoint("cleanup-after-" + key.section('-', 0, 0) + '-' + phase)
                || !store.put(marker, receipt)) return false;
            if (!checkpoint("cleanup-" + key.section('-', 0, 0) + '-' + phase)) return false;
        }
        return true;
    };
    for (const auto &value : proposal.value("snapshots").toArray()) {
        const auto manifest = value.toObject();
        const QString id = manifest.value("snapshot_id").toString();
        const QString relative = "snapshots/" + id + "/manifest.json";
        const auto validate = [&] {
            const auto found = store.lookup(relative);
            if (found == RemoteLookup::Unknown) return false;
            if (found == RemoteLookup::Missing) return true;
            QJsonObject observed;
            if (!store.get(relative, &observed) || observed != manifest) { store.error = "Snapshot changed since authorization."; return false; }
            return true;
        };
        if (!remove(store.remote(relative), "snapshot-" + id, validate)) return failed();
    }
    for (const auto &value : proposal.value("legacy").toArray()) {
        const auto copy = value.toObject();
        const QString path = copy.value("path").toString();
        const auto validate = [&] {
            const auto found = provider.lookup(path, &store.error);
            if (found == RemoteLookup::Unknown) return false;
            if (found == RemoteLookup::Missing) return true;
            QJsonObject observed;
            RemoteCopy verified;
            return BackupCatalog::verifyCopy(provider, path, request.legacySetId.isEmpty() ? request.scan.setId : request.legacySetId, &verified, &store.error)
                && verified.complete() && externalJson(store, provider, path + "/manifest.json", &observed)
                && observed == copy.value("manifest").toObject();
        };
        if (!remove(path, "legacy-" + digest(path.toUtf8()), validate)) return failed();
    }
    for (const auto &value : proposal.value("objects").toArray()) {
        const auto archive = value.toObject();
        const QString id = archive.value("id").toString();
        const auto validate = [&] {
            CleanupInventory fresh;
            bool successful = false;
            if (!cleanupInventory(store, provider, request, &fresh)
                || !terminalOperations(store, provider, request, history, &successful)) return false;
            if (fresh.liveObjects.contains(id)) { store.error = "An authorized object is still referenced; cleanup stopped."; return false; }
            const auto found = store.lookup(archive.value("remote").toString());
            if (found == RemoteLookup::Unknown) return false;
            if (found == RemoteLookup::Missing) return true;
            if (health(store, provider, archive, true) != Health::Good) { store.error = "Deletion candidate identity/content changed."; return false; }
            return true;
        };
        if (!remove(store.remote(archive.value("remote").toString()), "object-" + id, validate)) return failed();
    }
    if (!store.put(root + "/done.json", receipt) || !checkpoint("cleanup-done")) return failed();
    error->clear();
    return true;
}

bool IncrementalRepository::recoverCleanup(const IncrementalWriteRequest &request, BackupProvider &provider, QString *error)
{
    QString reason;
    if (!error) error = &reason;
    auto recovery = request;
    {
        Store store(request, provider);
        if (!store.open()) { *error = store.error; return false; }
        provider.beginBackupOperation();
        const auto end = qScopeGuard([&] { provider.endBackupOperation(); });
        QMap<QString, QJsonObject> retired;
        QString active;
        if (!store.owner() || !cleanupRecords(store, request, &retired, &active)) { *error = store.error; return false; }
        if (active.isEmpty()) return true;
        QJsonObject proposal;
        if (!store.get("maintenance/" + active + "/proposal.json", &proposal)) { *error = store.error; return false; }
        const auto authorization = store.lookup("maintenance/" + active + "/authorized.json");
        if (authorization == RemoteLookup::Unknown) { *error = store.error; return false; }
        recovery.cleanupConfirmation.clear();
        recovery.cleanupDismissal.clear();
        if (authorization == RemoteLookup::Missing) recovery.cleanupDismissal = digest(json(proposal));
        // Found means the existing executor must validate and resume the exact
        // durable authorization; it never substitutes a fresh retention policy.
    }
    QJsonObject proposal;
    QString confirmation;
    return cleanup(recovery, provider, &proposal, &confirmation, error);
}

IncrementalWriteResult IncrementalRepository::execute(const IncrementalWriteRequest &request, BackupProvider &provider)
{
    const auto integrity = checkIntegrity(request, provider);
    QSet<QString> knownDamage;
    for (const auto &value : integrity.value("known_damaged_objects").toArray()) knownDamage.insert(value.toString());
    for (const auto &value : integrity.value("damaged_objects").toArray()) knownDamage.insert(value.toString());
    IncrementalWriteResult result;
    if (integrity.value("reuse_blocked").toBool(true)) {
        result.state = IncrementalWriteResult::State::Retrying;
        result.lastSuccessfulCheck = request.scan.lastSuccessfulCheck;
        result.issues.append({request.remoteSetRoot, "integrity", "Unable to recover durable corruption knowledge; backup reuse is forbidden."});
    } else result = executeWrite(request, provider, knownDamage);
    result.integrity = integrity;
    for (const auto &value : integrity.value("damaged_objects").toArray())
        if (!result.damagedObjects.contains(value.toString())) result.damagedObjects.append(value.toString());
    for (const auto &value : integrity.value("affected_snapshots").toArray())
        if (!result.affectedSnapshots.contains(value.toString())) result.affectedSnapshots.append(value.toString());
    if (integrity.value("cleanup_blocked").toBool(true)) {
        result.cleanupNeedsAttention = true;
        result.issues.append({request.remoteSetRoot, "integrity", "Full integrity maintenance is incomplete; cleanup is blocked. " + integrity.value("error").toString()});
    }
    if (result.state == IncrementalWriteResult::State::Saved && request.retention > 0) {
        QString error;
        if (!cleanup(request, provider, &result.cleanupProposal, &result.cleanupConfirmation, &error)) {
            result.cleanupNeedsAttention = true;
            result.issues.append({request.remoteSetRoot, "cleanup", "Backup saved; cleanup needs attention: " + error});
        }
    }
    return result;
}

bool IncrementalRepository::takeover(const IncrementalWriteRequest &current, BackupProvider &provider,
    const QString &newComputerId, bool oldWriterPermanentlyDisabled, QString *error)
{
    QString reason;
    if (!error) error = &reason;
    if (!oldWriterPermanentlyDisabled || !IncrementalManifest::validId(newComputerId)
        || newComputerId == current.computerId || current.ownershipEpoch >= 9007199254740991LL) {
        *error = "Offline handoff requires a fresh computer identity and confirmation that the old writer cannot restart. No server-side fence exists.";
        return false;
    }
    Store store(current, provider);
    const auto failed = [&] { *error = store.error; return false; };
    if (!store.open()) return failed();
    provider.beginBackupOperation();
    const auto end = qScopeGuard([&] { provider.endBackupOperation(); });
    if (!store.owner()) return failed();
    QMap<QString, QJsonObject> retired;
    QString activeCleanup;
    if (!cleanupRecords(store, current, &retired, &activeCleanup)) return failed();
    if (!activeCleanup.isEmpty()) { *error = "Finish the exact pending cleanup before offline handoff."; return false; }
    const auto integrity = store.lookup("integrity");
    if (integrity == RemoteLookup::Unknown) return failed();
    if (integrity == RemoteLookup::Found) {
        QStringList cycles;
        store.completeListings = true;
        if (!store.children("integrity", &cycles)) return failed();
        for (const auto &id : cycles) {
            if (!IncrementalManifest::validId(id)) { *error = "Unknown integrity ledger blocks handoff."; return false; }
            const auto complete = store.lookup("integrity/" + id + "/complete.json");
            const auto cancelled = store.lookup("integrity/" + id + "/cancelled.json");
            if (complete == RemoteLookup::Unknown || cancelled == RemoteLookup::Unknown) return failed();
            if ((complete == RemoteLookup::Found) == (cancelled == RemoteLookup::Found)) {
                *error = "Finish or cancel integrity maintenance before offline handoff."; return false;
            }
        }
    }
    QStringList operations;
    if (!store.children("operations", &operations)) return failed();
    for (const auto &id : operations) {
        if (!IncrementalManifest::validId(id)) { *error = "Unknown operation blocks handoff."; return false; }
        const QString root = "operations/" + id;
        const auto complete = store.lookup(root + "/complete.json");
        const auto cancelled = store.lookup(root + "/cancelled.json");
        if (complete == RemoteLookup::Unknown || cancelled == RemoteLookup::Unknown) return failed();
        if ((complete == RemoteLookup::Found) == (cancelled == RemoteLookup::Found)) {
            *error = "Reconcile or explicitly cancel every protected operation before offline handoff: " + id;
            return false;
        }
        QJsonObject record;
        if (complete == RemoteLookup::Found) {
            QJsonObject identity, manifest;
            if (!store.get(root + "/identity.json", &identity) || !validPriorIdentity(identity, id, current)
                || !store.get(root + "/complete.json", &record)
                || !validCompletion(record, id, identity.value("snapshot_id").toString(), &store.error)
                || !(retired.contains(record.value("snapshot_id").toString())
                    ? (manifest = retired.value(record.value("snapshot_id").toString()), true)
                    : store.get("snapshots/" + record.value("snapshot_id").toString() + "/manifest.json", &manifest))
                || !IncrementalManifest::validate(json(manifest), current.scan.setId, record.value("snapshot_id").toString(), &store.error)
                || manifest.value("origin_computer_id") != current.originComputerId
                || manifest.value("created_at") != record.value("last_saved_change")) {
                if (store.error.isEmpty()) store.error = "Unreconciled completion blocks handoff.";
                return failed();
            }
        } else if (!store.get(root + "/cancelled.json", &record)
            || record != QJsonObject{{"version", 1}, {"operation_id", id}, {"state", "cancelled"}}) {
            *error = "Invalid cancellation blocks handoff.";
            return false;
        }
    }
    const auto maintenance = store.lookup("maintenance");
    if (maintenance == RemoteLookup::Unknown) return failed();
    if (maintenance == RemoteLookup::Found) {
        QMap<QString, QJsonObject> retired;
        QString active;
        if (!cleanupRecords(store, current, &retired, &active)) return failed();
        if (!active.isEmpty()) { *error = "Maintenance/deletion records require reconciliation before handoff."; return false; }
    }
    auto next = current;
    next.computerId = newComputerId;
    ++next.ownershipEpoch;
    const auto replacement = anchor(next);
    // Durable local intent precedes the only mutable control-record upload. A
    // lost response is accepted only if fresh readback proves the exact anchor.
    if (!store.local("handoff.json", QJsonObject{{"before", anchor(current)}, {"after", replacement}})
        || !store.owner()) return failed();
    QTemporaryDir temporary;
    const QString path = temporary.filePath("repository.json");
    if (!temporary.isValid() || !save(path, replacement, &store.error)) return failed();
    provider.upload(path, store.remote("repository.json"), &store.error);
    QJsonObject observed;
    if (!store.get("repository.json", &observed) || observed != replacement) {
        *error = "Ownership update is uncertain or conflicting; remain read-only and inspect repository.json. " + store.error;
        return false;
    }
    error->clear();
    return true;
}

#include "incrementalworker.h"
#include "incrementalmanifest.h"
#include "backupcatalog.h"
#include "incrementalrestore.h"
#include "backupstaging.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>

namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString fingerprint(const QString &directory)
{
    return BackupConfigStore::installationBinding(directory);
}
QString localPath(const QString &directory, const QString &root)
{
    return QDir(directory).filePath("incremental-writers/"
        + QString::fromLatin1(QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha256).toHex()) + ".json");
}
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
bool save(const QString &path, const QJsonObject &object, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return fail(error, "Unable to create writer state directory.");
    QSaveFile file(path);
    const auto bytes = QJsonDocument(object).toJson();
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(bytes) != bytes.size() || !file.commit()) return fail(error, "Unable to durably save writer identity.");
    return true;
}
bool read(const QString &path, QJsonObject *object, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, "Writer state is unreadable; explicit offline takeover is required.");
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (file.error() != QFileDevice::NoError || !document.isObject()) return fail(error, "Malformed writer state; refusing to replace its identity.");
    *object = document.object();
    return true;
}
QString prefix(const QString &path)
{
    QString name = QFileInfo(path).fileName();
    for (auto &character : name) if (!character.isLetterOrNumber() && character != '.' && character != '-' && character != '_') character = '_';
    return name.isEmpty() ? "source" : name;
}
QString journalRoot(const QString &stagingRoot, const QString &configurationDirectory)
{
    return QDir(stagingRoot).filePath("incremental-journals/" + fingerprint(configurationDirectory));
}

QStringList coordinationDirectories(const QString &configPath)
{
    if (configPath.isEmpty()) return {};
    const QDir directory(QFileInfo(configPath).absolutePath());
    return {directory.filePath("incremental"), directory.filePath("incremental-writers"), directory.filePath("continuations")};
}

bool historicalScopes(const BackupSet &set, const QJsonObject &anchor, BackupProvider &provider,
    QStringList *roots, QString *error)
{
    QVector<RemoteCopy> history;
    QString discoveryError;
    if (!BackupCatalog::discover(provider, set.remoteRoot, &history, &discoveryError) || !discoveryError.isEmpty())
        return fail(error, "Unable to recover legacy namespace provenance: " + discoveryError);
    for (const auto &copy : history) {
        QString shared, id, snapshot;
        if ((copy.setId == set.id || (copy.manifestVersion == 1 && copy.setName == QFileInfo(set.remoteFolder("computer")).fileName()))
            && !IncrementalRestore::snapshotLocation(copy.rootPath, &shared, &id, &snapshot))
            roots->append(QFileInfo(copy.rootPath).path());
    }
    QVector<RemoteItem> repository;
    if (!provider.listComplete(IncrementalWorker::root(set), &repository, error)) return false;
    bool maintenance = false;
    for (const auto &child : repository) if (child.name == "maintenance" && child.directory
        && child.path == IncrementalWorker::root(set) + "/maintenance") maintenance = true;
    if (maintenance) {
        QVector<RemoteItem> proposals;
        if (!provider.listComplete(IncrementalWorker::root(set) + "/maintenance", &proposals, error)) return false;
        for (const auto &item : proposals) {
            if (!item.directory || !IncrementalManifest::validId(item.name)
                || item.path != IncrementalWorker::root(set) + "/maintenance/" + item.name)
                return fail(error, "Ambiguous maintenance namespace during takeover.");
            QVector<RemoteItem> files;
            if (!provider.listComplete(item.path, &files, error)) return false;
            if (files.isEmpty()) continue;
            bool published = false;
            for (const auto &file : files) if (file.name == "proposal.json" && !file.directory && file.path == item.path + "/proposal.json") published = true;
            if (!published) return fail(error, "Unpublished maintenance records require manual reconciliation.");
            QTemporaryDir temporary;
            const auto local = temporary.filePath("proposal.json");
            QJsonObject proposal;
            if (!temporary.isValid() || !provider.download(item.path + "/proposal.json", local, error) || !read(local, &proposal, error)) return false;
            const auto owner = proposal.value("anchor").toObject();
            if (owner.value("application") != "omacustos" || owner.value("coordination") != "manual-offline-v1"
                || owner.value("set_id") != IncrementalWorker::setId(set)
                || owner.value("origin_computer_id") != anchor.value("origin_computer_id")
                || !IncrementalManifest::validId(owner.value("owner_computer_id").toString())
                || owner.value("ownership_epoch").toInteger() < 1
                || owner.value("ownership_epoch").toInteger() > anchor.value("ownership_epoch").toInteger()
                || !proposal.value("legacy").isArray())
                return fail(error, "Foreign maintenance provenance; takeover refused.");
            // Completed/dismissed proposals remain part of the repository's
            // provenance even when their old private copy folders are gone.
            for (const auto &value : proposal.value("legacy").toArray()) {
                const auto copy = value.toObject();
                const auto path = copy.value("path").toString();
                if (!value.isObject() || copy.size() != 2 || path != QDir::cleanPath(path)
                    || !path.startsWith(QDir::cleanPath(set.remoteRoot) + '/') || path.contains('\\')
                    || path.contains(QChar::Null) || path.split('/').contains("..") || !copy.value("manifest").isObject())
                    return fail(error, "Unsafe archived legacy namespace during takeover.");
                const auto manifest = temporary.filePath("legacy.json");
                if (!save(manifest, copy.value("manifest").toObject(), error)) return false;
                QVector<BackupEntry> entries;
                BackupManifestInfo info;
                if (!BackupManifest::load(manifest, &entries, &info, error)) return false;
                if (info.version == 1 && !BackupManifest::identifyLegacyCopy(path, set.id, entries, &info))
                    return fail(error, "Unidentifiable archived legacy copy.");
                if (info.setId != set.id || info.copyId != QFileInfo(path).fileName() || info.status != "complete")
                    return fail(error, "Archived legacy manifest is not from this configured set.");
                for (const auto &entry : entries) if (!entry.remotePath.startsWith(path + '/')
                    || (!entry.archive.id.isEmpty() && !entry.archive.remotePath.startsWith(path + '/')))
                    return fail(error, "Archived legacy payload escapes its private copy.");
                roots->append(QFileInfo(path).path());
            }
        }
    }
    roots->removeDuplicates(); roots->sort();
    return true;
}
}

QString IncrementalWorker::setId(const BackupSet &set)
{
    if (IncrementalManifest::validId(set.id)) return set.id;
    // Historical configurations used IDs such as "default". Keep their new
    // namespace deterministic, including after loss of local settings.
    auto bytes = QCryptographicHash::hash(("omacustos-set:" + set.id).toUtf8(), QCryptographicHash::Sha256).left(16);
    bytes[6] = char((static_cast<unsigned char>(bytes[6]) & 0x0f) | 0x40);
    bytes[8] = char((static_cast<unsigned char>(bytes[8]) & 0x3f) | 0x80);
    return QUuid::fromRfc4122(bytes).toString(QUuid::WithoutBraces);
}

QString IncrementalWorker::root(const BackupSet &set)
{
    return QDir::cleanPath(set.remoteRoot) + "/sets/" + setId(set);
}

bool IncrementalWorker::request(const BackupSet &set, const QString &stateDirectory,
    const QString &snapshotId, BackupProvider &provider, IncrementalWriteRequest *request, QString *error, bool provision,
    const std::function<bool(const QString &, QString *)> &stagingReady)
{
    if (!request || fingerprint(stateDirectory).isEmpty()) return fail(error, "A stable installation identity is required before writing backups.");
    if (set.readOnly) return fail(error, "This imported configuration is read-only. Explicit offline takeover is required.");
    BackupStaging staging;
    if (!staging.open(set.stagingDirectory, set.sourceDirectories, error)) return false;
    if (stagingReady && !stagingReady(staging.root(), error)) return false;
    const QString path = localPath(stateDirectory, root(set));
    QJsonObject state;
    if (QFile::exists(path)) {
        if (!read(path, &state, error)) return false;
        if (state.value("version") != 1 || state.value("remote_root") != root(set)
            || state.value("installation") != fingerprint(stateDirectory)
            || !IncrementalManifest::validId(state.value("computer_id").toString())
            || !IncrementalManifest::validId(state.value("origin_computer_id").toString())
            || state.value("ownership_epoch").toInteger() < 1)
            return fail(error, "Writer identity is foreign or invalid; explicit offline takeover is required.");
    } else {
        if (!provision) return fail(error, "No local writer authority. Explicit offline takeover is required.");
        const auto computer = uuid();
        state = {{"version", 1}, {"remote_root", root(set)}, {"installation", fingerprint(stateDirectory)},
            {"computer_id", computer}, {"origin_computer_id", computer}, {"ownership_epoch", 1},
            {"legacy_root", set.remoteFolder(QSysInfo::machineHostName())}};
        // Persist intent BEFORE touching the provider, so a lost initialization
        // response retries the same owner instead of provisioning a new one.
        if (!save(path, state, error)) return false;
    }
    request->remoteSetRoot = root(set);
    request->computerId = state.value("computer_id").toString();
    request->originComputerId = state.value("origin_computer_id").toString();
    request->ownershipEpoch = state.value("ownership_epoch").toInteger();
    request->operationId = snapshotId;
    request->snapshotId = snapshotId;
    request->scan.setId = setId(set);
    request->scan.checkedAt = QDateTime::currentDateTimeUtc();
    request->stateDirectory = journalRoot(staging.root(), stateDirectory);
    request->stagingDirectory = staging.root();
    request->stagingBudget = set.stagingBudget;
    request->retention = set.retention;
    request->legacyRoot = state.value("legacy_root").toString();
    request->legacySetId = set.id;
    if (provision && !IncrementalRepository::initialize(*request, provider, true, error)) return false;
    if (!state.value("legacy_roots").isArray()) {
        QVector<RemoteCopy> copies;
        QString discoveryError;
        if (!BackupCatalog::discover(provider, set.remoteRoot, &copies, &discoveryError) || !discoveryError.isEmpty())
            return fail(error, "Unable to establish historical namespaces safely: " + discoveryError);
        QStringList roots;
        for (const auto &copy : copies) {
            QString shared, id, snapshot;
            if ((copy.setId == set.id || (copy.manifestVersion == 1 && copy.setName == QFileInfo(set.remoteFolder("computer")).fileName()))
                && !IncrementalRestore::snapshotLocation(copy.rootPath, &shared, &id, &snapshot))
                roots.append(QFileInfo(copy.rootPath).path());
        }
        roots.removeDuplicates(); roots.sort();
        state["legacy_roots"] = QJsonArray::fromStringList(roots);
        if (!save(path, state, error)) return false;
    }
    for (const auto &root : state.value("legacy_roots").toArray()) {
        if (!root.isString() || !root.toString().startsWith(QDir::cleanPath(set.remoteRoot) + '/'))
            return fail(error, "Writer state contains a foreign historical namespace.");
        request->legacyRoots.append(root.toString());
    }
    // Absence must come from complete parent listings, never a failed download
    // or provider error. A new set need not have any legacy namespace at all.
    if (!request->legacyRoot.isEmpty()) {
        const auto remoteRoot = QDir::cleanPath(set.remoteRoot);
        const auto relative = QDir(remoteRoot).relativeFilePath(request->legacyRoot);
        if (relative.startsWith("../") || relative == ".." || relative == "." || QDir::isAbsolutePath(relative))
            return fail(error, "Writer state contains an unsafe legacy namespace.");
        auto parent = remoteRoot;
        for (const auto &segment : relative.split('/')) {
            QVector<RemoteItem> children;
            if (!provider.listComplete(parent, &children, error)) return false;
            bool found = false;
            QSet<QString> names;
            for (const auto &child : children) {
                if (child.name.isEmpty() || child.name.contains('/') || child.name == "." || child.name == ".."
                    || child.path != parent + '/' + child.name || names.contains(child.name))
                    return fail(error, "Ambiguous provider listing while locating legacy history.");
                names.insert(child.name);
                if (child.name == segment) {
                    if (!child.directory) return fail(error, "Legacy history path is not a directory.");
                    found = true;
                }
            }
            if (!found) { request->legacyRoot.clear(); break; }
            parent += '/' + segment;
        }
    }
    return true;
}

IncrementalWriteResult IncrementalWorker::run(const BackupSet &set, IncrementalWriteRequest request,
    BackupProvider &provider, const std::function<IncrementalControl(const QString &)> &control,
    const std::function<void(const QString &)> &warning, const QString &configPath)
{
    request.checkpoint = control;
    request.cleanupConfirmation.clear();
    request.cleanupDismissal.clear();
    QString cleanupError;
    if (!IncrementalRepository::recoverCleanup(request, provider, &cleanupError)) {
        IncrementalWriteResult failed;
        failed.state = IncrementalWriteResult::State::Retrying;
        failed.cleanupNeedsAttention = true;
        failed.issues.append({request.remoteSetRoot, "cleanup-recovery", cleanupError});
        return failed;
    }
    const auto stopped = [&] { return control && control("scanning") != IncrementalControl::Continue; };
    BackupEngine scanner;
    auto selection = scanner.selectSources(set.sourceDirectories, set.exclusions + coordinationDirectories(configPath), stopped, set.inclusions);
    excludeCoordinationFiles(&selection, configPath);
    if (stopped()) {
        // Let the repository durably reconcile cancel/publication even when no
        // source scan is available. Never pass an incomplete scan as deletions.
        request.checkpoint = control;
        return IncrementalRepository::execute(request, provider);
    }
    for (const auto &path : selection.scanErrors + selection.missingPaths)
        request.scan.scanIssues.append({path, "selection", "The selected source could not be scanned completely."});
    for (const auto &path : selection.skippedPaths) {
        const QFileInfo file(path);
        // Links and special nodes are intentionally outside the regular-file
        // selection. Permission failures and disappearing entries are not.
        if (!file.isSymLink() && (!file.exists() || (file.isFile() && !file.isReadable())))
            request.scan.scanIssues.append({path, "selection", "The selected file is unreadable or disappeared during scanning."});
    }
    if (set.sourceDirectories.isEmpty()) request.scan.scanIssues.append({{}, "selection", "No source roots are configured."});
    QStringList roots;
    for (const auto &source : set.sourceDirectories) roots.append(QDir::cleanPath(QFileInfo(source).absoluteFilePath()));
    // Root order must not change logical identities. Reserve naturally named
    // prefixes before allocating collision suffixes.
    auto sortedRoots = roots;
    sortedRoots.removeDuplicates();
    sortedRoots.sort();
    QSet<QString> reserved, allocated;
    QHash<QString, QString> mapping;
    for (const auto &source : sortedRoots) reserved.insert(prefix(source));
    for (const auto &source : sortedRoots) {
        const auto base = prefix(source);
        auto name = base;
        int suffix = 2;
        while (allocated.contains(name)) {
            do { name = base + '-' + QString::number(suffix++); } while (reserved.contains(name) || allocated.contains(name));
        }
        allocated.insert(name);
        mapping.insert(source, name);
    }
    for (const auto &path : selection.includedFiles) {
        QString source;
        for (const auto &candidate : sortedRoots) if (path == candidate || path.startsWith(candidate + '/')) { source = candidate; break; }
        if (source.isEmpty()) {
            request.scan.scanIssues.append({path, "selection", "A selected file is outside all configured roots."});
            continue;
        }
        auto relative = QDir(source).relativeFilePath(path);
        if (relative == ".") relative = QFileInfo(path).fileName();
        if (sortedRoots.size() > 1) relative = mapping.value(source) + '/' + relative;
        request.scan.sources.append({path, relative});
    }
    QString capacity;
    QVector<RemoteItem> snapshots;
    QString inspectionError;
    if (!provider.list(request.remoteSetRoot + "/snapshots", &snapshots, &inspectionError)) {
        IncrementalWriteResult failed;
        failed.state = IncrementalWriteResult::State::Retrying;
        failed.issues.append({request.remoteSetRoot, "capacity", "Unable to establish whether a fresh baseline is required: " + inspectionError});
        return failed;
    }
    if (snapshots.isEmpty()) {
        QVector<RemoteCopy> history;
        qint64 existing = 0;
        QSet<QString> objects;
        const bool known = BackupCatalog::discover(provider, set.remoteRoot, &history, &inspectionError) && inspectionError.isEmpty();
        if (known) for (const auto &copy : history) if (copy.setId == set.id || copy.setId == setId(set)) {
            for (const auto &entry : copy.entries) {
                const auto path = entry.archive.id.isEmpty() ? entry.remotePath : entry.archive.remotePath;
                if (objects.contains(path)) continue;
                objects.insert(path);
                existing += entry.archive.id.isEmpty() ? entry.size : entry.archive.size;
            }
        }
        capacity = QStringLiteral("Fresh incremental baseline: keep all old history (%1 payload bytes) PLUS approximately %2 selected, uncompressed source bytes. Compression and metadata change actual usage; this is not a quota guarantee. History will never be removed to make space.")
            .arg(known ? QString::number(existing) : QStringLiteral("unknown"))
            .arg(selection.totalBytes);
        if (warning) warning(capacity);
    }
    auto result = IncrementalRepository::execute(request, provider);
    if (!result.cleanupProposal.isEmpty() && !result.cleanupConfirmation.isEmpty()) {
        // Scheduled runs may present attention, never grant deletion authority.
        // Retire their unapproved preview so a deferred human review does not
        // prevent subsequent backups. The UI obtains a fresh exact proposal.
        auto dismissal = request;
        dismissal.cleanupDismissal = result.cleanupConfirmation;
        QString error, digest;
        QJsonObject proposal;
        if (!IncrementalRepository::cleanup(dismissal, provider, &proposal, &digest, &error))
            result.issues.append({request.remoteSetRoot, "cleanup", "Unapproved cleanup preview needs reconciliation: " + error});
    }
    result.capacityWarning = capacity;
    return result;
}

void IncrementalWorker::excludeCoordinationFiles(BackupPreview *selection, const QString &configPath)
{
    if (!selection || configPath.isEmpty()) return;
    const auto directory = QFileInfo(configPath).absolutePath();
    const auto excludedDirectory = coordinationDirectories(configPath);
    const QStringList files{"omacustos-backup-runs.json", "omacustos-backup-cleanup.json",
        QFileInfo(configPath).fileName() + ".worker.lock", QFileInfo(configPath).fileName() + ".lock"};
    const auto internal = [&](const QString &path) {
        const auto absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        for (const auto &root : excludedDirectory) if (absolute == root || absolute.startsWith(root + '/')) return true;
        if (QFileInfo(absolute).absolutePath() != directory) return false;
        const auto name = QFileInfo(absolute).fileName();
        for (const auto &file : files) if (name == file || name.startsWith(file + '.')) return true;
        return false;
    };
    for (auto *paths : {&selection->includedFiles, &selection->skippedPaths, &selection->missingPaths, &selection->scanErrors}) {
        paths->erase(std::remove_if(paths->begin(), paths->end(), [&](const auto &path) {
            if (!internal(path)) return false;
            selection->excludedFiles.append(path);
            return true;
        }), paths->end());
    }
    selection->excludedFiles.removeDuplicates(); selection->excludedFiles.sort();
    selection->totalBytes = 0;
    for (const auto &path : selection->includedFiles) selection->totalBytes += qMax(qint64(0), QFileInfo(path).size());
}

bool IncrementalWorker::takeover(const BackupSet &set, const QString &stateDirectory, BackupProvider &provider,
    bool oldWriterDisabled, QString *error)
{
    if (!oldWriterDisabled || fingerprint(stateDirectory).isEmpty()) return fail(error, "Permanently disable the old writer before confirming offline takeover.");
    BackupStaging staging;
    if (!staging.open(set.stagingDirectory, set.sourceDirectories, error)) return false;
    const QString path = localPath(stateDirectory, root(set));
    // Fetch CURRENT remote authority; never trust a copied local epoch.
    QTemporaryDir temporary;
    const auto local = temporary.filePath("repository.json");
    QJsonObject anchor;
    if (!temporary.isValid()) return fail(error, "Unable to create private takeover workspace.");
    const bool downloaded = provider.download(root(set) + "/repository.json", local, error);
    if (downloaded && !read(local, &anchor, error)) return false;
    const auto enrollmentPath = path + ".initialize";
    if (!downloaded || QFile::exists(enrollmentPath)) {
        QJsonObject enrollment;
        if (QFile::exists(enrollmentPath)) {
            if (!read(enrollmentPath, &enrollment, error)) return false;
            if (enrollment.value("installation") != fingerprint(stateDirectory) || enrollment.value("remote_root") != root(set)
                || !IncrementalManifest::validId(enrollment.value("computer_id").toString())
                || enrollment.value("origin_computer_id") != enrollment.value("computer_id")
                || enrollment.value("ownership_epoch") != 1)
                return fail(error, "Foreign or invalid owner-enrollment intent; initialization refused.");
        } else {
            const auto computer = uuid();
            enrollment = {{"version", 1}, {"remote_root", root(set)}, {"installation", fingerprint(stateDirectory)},
                {"computer_id", computer}, {"origin_computer_id", computer}, {"ownership_epoch", 1},
                {"legacy_root", set.remoteFolder(QSysInfo::machineHostName())}};
            if (!save(enrollmentPath, enrollment, error)) return false;
        }
        const QJsonObject expected{{"version", 1}, {"application", "omacustos"}, {"coordination", "manual-offline-v1"},
            {"set_id", setId(set)}, {"origin_computer_id", enrollment.value("computer_id")},
            {"owner_computer_id", enrollment.value("computer_id")}, {"ownership_epoch", 1}};
        if (!downloaded) {
            IncrementalWriteRequest initial;
            initial.remoteSetRoot = root(set); initial.scan.setId = setId(set);
            initial.scan.checkedAt = QDateTime::currentDateTimeUtc();
            initial.computerId = initial.originComputerId = enrollment.value("computer_id").toString();
            initial.operationId = initial.snapshotId = uuid();
            initial.stateDirectory = journalRoot(staging.root(), stateDirectory);
            initial.stagingDirectory = staging.root(); initial.stagingBudget = set.stagingBudget;
            // A failed download is NOT absence evidence. initialize independently
            // requires a complete empty listing, or the exact existing anchor.
            if (!IncrementalRepository::initialize(initial, provider, true, error)) return false;
            anchor = expected;
        }
        if (anchor == expected) {
            if (!save(path, enrollment, error)) return false;
            if (!QFile::remove(enrollmentPath)) return fail(error, "Owner enrollment saved; its intent needs local reconciliation.");
            return true;
        }
    }
    const QSet<QString> anchorFields{"version", "application", "coordination", "set_id", "origin_computer_id", "owner_computer_id", "ownership_epoch"};
    const auto anchorKeys = anchor.keys();
    if (QSet<QString>(anchorKeys.cbegin(), anchorKeys.cend()) != anchorFields
        || anchor.value("version") != 1 || anchor.value("application") != "omacustos"
        || anchor.value("coordination") != "manual-offline-v1" || anchor.value("set_id") != setId(set)
        || !IncrementalManifest::validId(anchor.value("origin_computer_id").toString())
        || !IncrementalManifest::validId(anchor.value("owner_computer_id").toString())
        || anchor.value("ownership_epoch").toInteger() < 1)
        return fail(error, "Invalid remote ownership record; takeover refused.");
    QStringList legacyRoots;
    if (!historicalScopes(set, anchor, provider, &legacyRoots, error)) return false;
    QJsonObject intent;
    // A durable handoff intent makes a lost takeover response recoverable.
    const QString intentPath = path + ".takeover";
    if (QFile::exists(intentPath)) {
        if (!read(intentPath, &intent, error)) return false;
        if (intent.value("installation") != fingerprint(stateDirectory) || intent.value("remote_root") != root(set))
            return fail(error, "Foreign takeover intent; manual recovery is required.");
    } else {
        intent = {{"version", 1}, {"remote_root", root(set)}, {"installation", fingerprint(stateDirectory)},
            {"computer_id", uuid()}, {"origin_computer_id", anchor.value("origin_computer_id")},
            {"ownership_epoch", anchor.value("ownership_epoch").toInteger() + 1},
            {"legacy_root", set.remoteFolder(QSysInfo::machineHostName())}};
        if (!save(intentPath, intent, error)) return false;
    }
    intent["legacy_roots"] = QJsonArray::fromStringList(legacyRoots);
    if (!save(intentPath, intent, error)) return false;
    if (anchor.value("owner_computer_id") != intent.value("computer_id")
        || anchor.value("ownership_epoch") != intent.value("ownership_epoch")) {
        IncrementalWriteRequest current;
        current.remoteSetRoot = root(set);
        current.scan.setId = setId(set);
        current.scan.checkedAt = QDateTime::currentDateTimeUtc();
        current.computerId = anchor.value("owner_computer_id").toString();
        current.originComputerId = anchor.value("origin_computer_id").toString();
        current.ownershipEpoch = anchor.value("ownership_epoch").toInteger();
        current.operationId = current.snapshotId = uuid();
        current.stateDirectory = journalRoot(staging.root(), stateDirectory);
        current.stagingDirectory = staging.root();
        current.stagingBudget = set.stagingBudget;
        current.legacyRoots = legacyRoots;
        current.legacySetId = set.id;
        if (current.ownershipEpoch + 1 != intent.value("ownership_epoch").toInteger()
            || current.originComputerId != intent.value("origin_computer_id").toString())
            return fail(error, "Remote ownership changed during takeover; manual reconciliation is required.");
        if (!IncrementalRepository::takeover(current, provider, intent.value("computer_id").toString(), true, error)) return false;
    }
    if (!save(path, intent, error)) return false;
    if (!QFile::remove(intentPath)) return fail(error, "Takeover saved; unable to remove its completed intent.");
    return true;
}

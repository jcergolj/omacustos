#include "backupengine.h"
#include "incrementalrepository.h"
#include "incrementalrestore.h"
#include "localprovider.h"
#include "backupcatalog.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QUuid>
#include <cstdio>
#include <dlfcn.h>

namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

qint64 treeBytes(const QString &path)
{
    qint64 bytes = 0;
    QDirIterator files(path, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (files.hasNext()) { files.next(); bytes += files.fileInfo().size(); }
    return bytes;
}

QByteArray digest(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? hash.result() : QByteArray();
}

void report(const QJsonObject &object)
{
    std::puts(QJsonDocument(object).toJson(QJsonDocument::Compact).constData());
    std::fflush(stdout);
}

// Deliberately size-only, as on the Proton CLI path. Verification downloads
// remain enabled. All mutations are confined to a fresh local fixture directory.
class MeasuredProvider final : public BackupProvider {
public:
    explicit MeasuredProvider(const QString &root) : root(root), local(root) {}
    QString root;
    LocalProvider local;
    QJsonObject counts, elapsedNs;
    qint64 uploadedBytes = 0, downloadedBytes = 0, objectUploadedBytes = 0;
    int objectUploads = 0;
    QStringList objectPaths;
    QString staging;
    qint64 checkpointStagingPeak = 0;

    void sample()
    {
        checkpointStagingPeak = qMax(checkpointStagingPeak, treeBytes(staging));
        static auto observe = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "omacustos_staging_observer_sample"));
        if (observe) observe();
    }
    void reset()
    {
        counts = {}; elapsedNs = {};
        uploadedBytes = downloadedBytes = objectUploadedBytes = 0;
        objectUploads = 0; objectPaths.clear(); checkpointStagingPeak = 0;
        static auto reset = reinterpret_cast<void (*)(const char *)>(dlsym(RTLD_DEFAULT, "omacustos_staging_observer_reset"));
        if (reset) reset(QFile::encodeName(QFileInfo(staging).absoluteFilePath()).constData());
    }
    template<class Function> auto measure(const QString &name, Function operation)
    {
        sample();
        QElapsedTimer clock;
        clock.start();
        const auto result = operation();
        elapsedNs[name] = elapsedNs.value(name).toInteger() + clock.nsecsElapsed();
        counts[name] = counts.value(name).toInt() + 1;
        sample();
        return result;
    }
    QJsonObject metrics() const
    {
        QJsonObject result{{"remote_operations", counts}, {"provider_elapsed_ns", elapsedNs},
            {"uploaded_bytes", uploadedBytes}, {"downloaded_bytes", downloadedBytes},
            {"object_uploaded_bytes", objectUploadedBytes}, {"object_uploads", objectUploads},
            {"boundary_sampled_private_staging_peak_bytes", checkpointStagingPeak}};
        static auto peak = reinterpret_cast<long long (*)()>(dlsym(RTLD_DEFAULT, "omacustos_staging_observer_peak"));
        if (peak) result["observed_private_staging_peak_bytes"] = qint64(peak());
        result["staging_observer_valid"] = !peak || peak() >= checkpointStagingPeak;
        return result;
    }
    bool trustworthyContentChecksums() const override { return false; }
    bool upload(const QString &source, const QString &remote, QString *error) override;
    bool uploadImmutable(const QString &source, const QString &remote, QString *error) override
    {
        return measure("upload", [&] {
            const bool result = local.uploadImmutable(source, remote, error);
            if (result) {
                const auto size = QFileInfo(source).size();
                uploadedBytes += size;
                if (remote.endsWith(".tar.gz")) { ++objectUploads; objectPaths.append(remote); objectUploadedBytes += size; }
            }
            return result;
        });
    }
    RemoteLookup lookup(const QString &path, QString *error) override
    { return measure("lookup", [&] { return local.lookup(path, error); }); }
    bool ensureDirectory(const QString &path, QString *error) override
    { return measure("mkdir", [&] { return local.ensureDirectory(path, error); }); }
    bool download(const QString &path, const QString &destination, QString *error) override
    {
        return measure("download", [&] {
            const bool result = local.download(path, destination, error);
            if (result) downloadedBytes += QFileInfo(destination).size();
            return result;
        });
    }
    bool inspect(const QString &path, RemoteFile *file, QString *error) override
    {
        return measure("inspect", [&] {
            const bool result = local.inspect(path, file, error);
            file->checksum.clear();
            return result;
        });
    }
    bool list(const QString &path, QVector<RemoteItem> *items, QString *error) override
    { return measure("list", [&] { return local.list(path, items, error); }); }
    bool listComplete(const QString &path, QVector<RemoteItem> *items, QString *error) override
    { return measure("list_complete", [&] { return local.listComplete(path, items, error); }); }
    bool deletionIdentity(const QString &path, QString *identity, QString *error) override
    { return measure("deletion_identity", [&] { return local.deletionIdentity(path, identity, error); }); }
    bool trashExact(const QString &path, const QString &identity, QString *error) override
    { return measure("trash_exact", [&] { return local.trashExact(path, identity, error); }); }
    bool deleteExact(const QString &path, const QString &identity, QString *error) override
    { return measure("delete_exact", [&] { return local.deleteExact(path, identity, error); }); }
    bool trash(const QString &, QString *) override { return false; }
    bool permanentlyDelete(const QString &, QString *) override { return false; }
};

bool MeasuredProvider::upload(const QString &source, const QString &remote, QString *error)
{
    return measure("upload", [&] {
        const bool success = local.upload(source, remote, error);
        if (success) {
            const auto size = QFileInfo(source).size();
            uploadedBytes += size;
            if (remote.endsWith(".tar.gz")) { ++objectUploads; objectPaths.append(remote); objectUploadedBytes += size; }
        }
        return success;
    });
}

bool generate(const QString &path, qint64 size, int index, bool mixed)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    quint32 random = 0x5a17c9e3 ^ quint32(index);
    int line = 0;
    QByteArray pending;
    while (size > 0) {
        QByteArray chunk;
        if (mixed && index % 2) {
            chunk.resize(qMin(size, qint64(65536)));
            for (char &byte : chunk) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; byte = char(random); }
        } else {
            chunk = QString("// Document %1 section %2\nconst record_%2 = { path: 'src/component/%1', enabled: true };\n").arg(index).arg(line++).toUtf8();
            chunk.truncate(qMin(size, qint64(chunk.size())));
        }
        pending.append(chunk);
        size -= chunk.size();
        if (pending.size() >= 65536 || size == 0) {
            if (file.write(pending) != pending.size()) return false;
            pending.clear();
        }
    }
    return file.flush();
}

QString errors(const IncrementalWriteResult &result)
{
    QStringList reasons;
    for (const auto &issue : result.issues) reasons.append(issue.phase + ": " + issue.reason);
    return reasons.join('\n');
}

QJsonObject inventoryMetrics(const QString &manifest)
{
    QFile file(manifest);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    QSet<QString> live;
    for (const auto &value : object.value("entries").toArray()) {
        const auto entry = value.toObject();
        live.insert(entry.value("archive").toString() + '/' + entry.value("member").toString());
    }
    qint64 physical = 0, obsolete = 0, compressed = 0;
    for (const auto &value : object.value("archives").toArray()) {
        const auto archive = value.toObject();
        compressed += archive.value("size").toInteger();
        for (const auto &memberValue : archive.value("members").toArray()) {
            const auto member = memberValue.toObject();
            const auto size = member.value("size").toInteger();
            physical += size;
            if (!live.contains(archive.value("id").toString() + '/' + member.value("path").toString())) obsolete += size;
        }
    }
    return {{"referenced_compressed_bytes", compressed}, {"physical_member_bytes", physical},
        {"obsolete_member_bytes", obsolete}};
}

// Compare against independently captured hashes, not merely a successful engine
// return or the current source (which changes after the first snapshot).
bool restore(MeasuredProvider &provider, const QString &snapshot, const QString &destination,
    const QMap<QString, QByteArray> &expected, bool single, QJsonObject *metrics, QString *error)
{
    provider.reset();
    QElapsedTimer clock;
    clock.start();
    RemoteCopy copy;
    if (!BackupCatalog::verifyCopy(provider, snapshot, {}, &copy, error)) return false;
    // Avoid selecting the newly edited file or the short final archive, either
    // of which can hide selective-download amplification at larger targets.
    const QString selectedPath = expected.keys().at(expected.size() / 2);
    QVector<BackupEntry> selected;
    for (const auto &entry : copy.entries)
        if (!single || entry.restorePath == selectedPath) selected.append(entry);
    const auto result = BackupEngine().restoreFiles({selected, destination, snapshot}, provider);
    if (!result.success) { *error = result.error; return false; }
    const qint64 elapsed = clock.nsecsElapsed();
    int verified = 0;
    for (auto iterator = expected.begin(); iterator != expected.end(); ++iterator) {
        if (single && iterator.key() != selectedPath) continue;
        if (digest(QDir(destination).filePath(iterator.key())) != iterator.value()) {
            *error = "Restored bytes differ: " + iterator.key(); return false;
        }
        ++verified;
    }
    int actualFiles = 0;
    QDirIterator files(destination, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (files.hasNext()) { files.next(); ++actualFiles; }
    if (actualFiles != verified || result.restoredCount != verified) {
        *error = "Restore placed unexpected files or omitted selected files."; return false;
    }
    *metrics = provider.metrics();
    if (!metrics->value("staging_observer_valid").toBool()) { *error = "Event observer underreported a known staging boundary."; return false; }
    (*metrics)["elapsed_ns"] = elapsed;
    (*metrics)["verified_files"] = verified;
    return true;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    bool bytesValid = false, countValid = false, targetValid = false;
    qint64 bytes = arguments.value(1).toLongLong(&bytesValid);
    int count = arguments.value(2).toInt(&countValid);
    const qint64 target = arguments.value(3).toLongLong(&targetValid);
    const QString requestedProfile = arguments.value(4);
    const bool production = requestedProfile.startsWith("production-");
    const QString profile = production ? requestedProfile.mid(11) : requestedProfile;
    if (arguments.size() != 5 || !bytesValid || !countValid || !targetValid || count < 4
        || bytes < count || target <= 0 || target > 1000LL * 1000 * 1000
        || (profile != "documents-code" && profile != "mixed" && profile != "repository-code" && profile != "repository-files")) {
        std::fprintf(stderr, "Usage: incremental-benchmark BYTES FILES TARGET_BYTES documents-code|mixed|repository-code|repository-files\n");
        return 2;
    }
    QStringList corpus;
    if (profile == "repository-code" || profile == "repository-files") {
        for (const auto &subdirectory : {QString("native/src"), QString("native/qml")}) {
            QDirIterator files(QDir(QStringLiteral(OMACUSTOS_SOURCE_DIR)).filePath(subdirectory),
                {"*.cpp", "*.h", "*.qml", "*.md"}, QDir::Files, QDirIterator::Subdirectories);
            while (files.hasNext()) corpus.append(files.next());
        }
        for (const auto &document : {QString("README.md"), QString("TECHNICAL.md")}) corpus.append(QDir(QStringLiteral(OMACUSTOS_SOURCE_DIR)).filePath(document));
        corpus.sort();
        if (corpus.isEmpty()) { std::fprintf(stderr, "Repository code corpus is empty.\n"); return 2; }
        if (profile == "repository-files") {
            count = corpus.size(); bytes = 0;
            for (const auto &path : corpus) bytes += QFileInfo(path).size();
        }
    }
    QTemporaryDir work(QDir::current().filePath("incremental-benchmark-XXXXXX"));
    if (!work.isValid() || QStorageInfo(work.path()).fileSystemType() == "tmpfs") {
        std::fprintf(stderr, "Run from a writable disk-backed working directory, not tmpfs.\n");
        return 2;
    }
    const auto fail = [](const QString &error) { std::fprintf(stderr, "%s\n", qPrintable(error)); return 1; };
    const auto temporary = work.filePath("temporary");
    if (!QDir().mkpath(temporary)) return fail("Unable to create private disk-backed temporary storage.");
    qputenv("TMPDIR", QFile::encodeName(temporary));
    if (QDir::tempPath() != temporary) return fail("Qt temporary storage was cached before the disk-backed fixture was selected.");
    // Keep room for independent restore trees, remote copies, and staging. This
    // conservative local-fixture estimate is not a remote quota guarantee.
    if (bytes > QStorageInfo(work.path()).bytesAvailable() / 8)
        return fail("Insufficient local fixture capacity: reserve eight times the selected bytes.");
    IncrementalWriteRequest request;
    request.scan.setId = uuid(); request.computerId = uuid(); request.originComputerId = request.computerId;
    request.remoteSetRoot = "repository/sets/" + request.scan.setId;
    request.stateDirectory = work.filePath("state"); request.stagingDirectory = work.filePath("staging");
    request.archiveTargetBytes = target;
    request.stagingBudget = production ? 1000 * 1000 * 1000 : qMax(4 * target, 4 * (bytes / count + 65536));
    request.batchFileLimit = production ? 1000 : count;
    request.scan.checkedAt = QDateTime::currentDateTimeUtc();
    request.operationId = uuid(); request.snapshotId = uuid();
    QDir().mkpath(work.filePath("sources")); QDir().mkpath(work.filePath("cloud"));
    QMap<QString, QByteArray> baseline;
    for (int index = 0; index < count; ++index) {
        const QString name = QString("file-%1").arg(index, 8, 10, QChar('0'));
        const QString path = work.filePath("sources/" + name);
        const auto size = bytes / count + (index < bytes % count ? 1 : 0);
        if (profile == "repository-files") {
            if (!QFile::copy(corpus.at(index), path)) return fail("Repository corpus copy failed.");
        } else if (profile == "repository-code") {
            QFile source(corpus.at(index % corpus.size())), output(path);
            if (!source.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) return fail("Unable to read repository code corpus.");
            auto code = source.readAll();
            if (code.isEmpty()) return fail("Empty repository code corpus member.");
            code += "\n// benchmark fixture " + QByteArray::number(index) + '\n';
            qint64 remaining = size;
            while (remaining > 0) {
                const auto length = qMin(remaining, qint64(code.size()));
                if (output.write(code.constData(), length) != length) return fail("Repository corpus amplification failed.");
                remaining -= length;
            }
        } else if (!generate(path, size, index, profile == "mixed")) return fail("Fixture generation failed.");
        const auto checksum = digest(path);
        if (checksum.isEmpty()) return fail("Fixture hashing failed.");
        baseline.insert(name, checksum); request.scan.sources.append({path, name});
    }
    MeasuredProvider provider(work.filePath("cloud"));
    provider.staging = request.stagingDirectory;
    if (qEnvironmentVariableIsSet("OMACUSTOS_REQUIRE_STAGING_OBSERVER")
        && !provider.metrics().contains("observed_private_staging_peak_bytes")) return fail("The required staging event observer is not loaded.");
    QString error;
    const QString firstSnapshot = request.remoteSetRoot + "/snapshots/" + request.snapshotId;
    QCryptographicHash fixtureHash(QCryptographicHash::Sha256);
    for (auto it = baseline.cbegin(); it != baseline.cend(); ++it) { fixtureHash.addData(it.key().toUtf8()); fixtureHash.addData(it.value()); }
    report({{"measurement", "local-fixture-not-cloud"}, {"profile", profile}, {"source_bytes", bytes},
        {"fixture_sha256", QString::fromLatin1(fixtureHash.result().toHex())}, {"production_constraints", production}, {"batch_file_limit", request.batchFileLimit},
        {"corpus_note", profile == "repository-files" ? "Unmodified current repository source/QML/docs; arguments BYTES/FILES overridden by actual corpus." : profile == "repository-code" ? "Current repository source/QML/docs amplified by repetition to the requested size; not a customer corpus." : "Deterministic synthetic fixture, not a customer corpus."},
        {"files", count}, {"archive_target_bytes", target}, {"staging_budget_bytes", request.stagingBudget},
        {"phase_note", "Boundary intervals include journals and provider work; provider times overlap them. Writer staging is sampled, not an exact high-water mark. Local inspect hashes bytes internally before discarding the checksum; not a cloud cost model."}});
    QString firstV3;
    const auto v3 = [&](const QString &scenario) {
        BackupOptions options;
        options.freshCopy = options.boundedArchives = true;
        options.archiveTargetBytes = target;
        options.stagingDirectory = work.filePath("v3-staging");
        options.stagingBudget = request.stagingBudget;
        options.batchFileLimit = request.batchFileLimit;
        options.continuationDirectory = work.filePath("v3-continuations/" + uuid());
        options.retainLocalManifest = false;
        const auto id = uuid();
        const auto root = "legacy-v3/" + id;
        provider.staging = options.stagingDirectory;
        provider.reset();
        if (firstV3.isEmpty()) firstV3 = root;
        BackupCopyMetadata metadata{"benchmark", request.scan.setId, "Benchmark", id, QDateTime::currentDateTimeUtc()};
        QString manifest;
        BackupResult result;
        QElapsedTimer clock; clock.start();
        const bool success = BackupEngine().backup({work.filePath("sources")}, root, {}, metadata,
            provider, &manifest, &error, {}, &result, options);
        auto metrics = provider.metrics();
        metrics["scenario"] = "v3-" + scenario;
        metrics["elapsed_ns"] = clock.nsecsElapsed();
        metrics["staging_note"] = "Private writer workspace bytes; optional event observer also tracks intermediate write growth.";
        if (!metrics.value("staging_observer_valid").toBool()) { error = "v3 event observer underreported a staging boundary."; return false; }
        report(metrics);
        provider.staging = request.stagingDirectory;
        return success;
    };
    if (!v3("first-run") || !v3("unchanged-run")) return fail(error);
    for (const QString &scenario : {QString("first-run"), QString("unchanged-run"), QString("small-edit")}) {
        if (scenario != "first-run") {
            request.operationId = uuid(); request.snapshotId = uuid();
            request.scan.checkedAt = request.scan.checkedAt.addSecs(1);
        }
        if (scenario == "small-edit") {
            QFile edit(request.scan.sources[0].sourcePath);
            if (!edit.open(QIODevice::Append) || edit.write("\n// small edit\n") != 15 || !edit.flush()) return fail("Edit failed.");
            edit.close();
            const auto renamed = request.scan.sources[1];
            if (!QFile::rename(renamed.sourcePath, renamed.sourcePath + "-renamed")) return fail("Rename failed.");
            request.scan.sources[1] = {renamed.sourcePath + "-renamed", renamed.restorePath + "-renamed"};
            if (!QFile::remove(request.scan.sources[2].sourcePath)) return fail("Deletion failed.");
            request.scan.sources.removeAt(2);
            if (!v3("small-edit")) return fail(error);
        }
        provider.reset();
        QJsonObject intervals;
        QJsonObject measuredPhases;
        request.phaseMeasured = [&](const QString &phase, qint64 elapsed) {
            measuredPhases[phase] = measuredPhases.value(phase).toInteger() + elapsed;
        };
        QElapsedTimer clock;
        clock.start();
        qint64 previous = 0;
        request.checkpoint = [&](const QString &phase) {
            const auto now = clock.nsecsElapsed();
            intervals[phase] = intervals.value(phase).toInteger() + now - previous;
            previous = now;
            // These control polls run per file/read. Walking a growing staging
            // tree here creates quadratic probe overhead for larger archives.
            if (phase != "capturing" && phase != "preparing") provider.sample();
            return IncrementalControl::Continue;
        };
        QElapsedTimer selectionTimer; selectionTimer.start();
        const auto selection = BackupEngine().selectSources({work.filePath("sources")}, {});
        measuredPhases["source-enumeration"] = selectionTimer.nsecsElapsed();
        if (!selection.scanErrors.isEmpty() || !selection.missingPaths.isEmpty() || !selection.skippedPaths.isEmpty()
            || selection.includedFiles.size() != request.scan.sources.size()) return fail("Incomplete source enumeration in benchmark.");
        if (scenario == "first-run" && !IncrementalRepository::initialize(request, provider, true, &error)) return fail(error);
        const auto result = IncrementalRepository::execute(request, provider);
        const auto elapsed = clock.nsecsElapsed();
        if (!result.success()) return fail(errors(result));
        if (scenario == "unchanged-run" && (result.state != IncrementalWriteResult::State::Unchanged
            || result.manifestCommitted || provider.objectUploads != 0)) return fail("Unchanged check saved or uploaded data.");
        if (scenario != "unchanged-run" && !result.manifestCommitted) return fail("Changed state was not published.");
        if (scenario == "small-edit") {
            if (provider.objectUploads != 1) return fail("Rename/deletion retransmitted unchanged data.");
            QFile manifest(QDir(provider.root).filePath(result.manifestPath));
            if (!manifest.open(QIODevice::ReadOnly)) return fail("Saved manifest could not be read.");
            const auto archives = QJsonDocument::fromJson(manifest.readAll()).object().value("archives").toArray();
            bool checkedNewArchive = false;
            for (const auto &value : archives) {
                const auto archive = value.toObject();
                if (request.remoteSetRoot + '/' + archive.value("remote").toString() != provider.objectPaths.first()) continue;
                const auto members = archive.value("members").toArray();
                if (members.size() != 1 || members.first().toObject().value("size").toInteger() != QFileInfo(request.scan.sources[0].sourcePath).size()
                    || members.first().toObject().value("sha256").toString() != QString::fromLatin1(digest(request.scan.sources[0].sourcePath).toHex()))
                    return fail("New archive must contain only the edited file, not unchanged payloads.");
                checkedNewArchive = true;
            }
            if (!checkedNewArchive) return fail("Newly uploaded archive was not referenced by the snapshot.");
        }
        auto metrics = provider.metrics();
        if (!metrics.value("staging_observer_valid").toBool()) return fail("v4 event observer underreported a staging boundary.");
        metrics["scenario"] = scenario; metrics["elapsed_ns"] = elapsed;
        metrics["boundary_intervals_ns"] = intervals; metrics["after_last_boundary_ns"] = elapsed - previous;
        metrics["measured_phases_ns"] = measuredPhases;
        metrics["remote_tree_bytes"] = treeBytes(provider.root);
        if (result.manifestCommitted) metrics["inventory"] = inventoryMetrics(QDir(provider.root).filePath(result.manifestPath));
        report(metrics);
    }
    request.checkpoint = {};
    request.phaseMeasured = {};
    provider.staging = temporary;
    for (const bool single : {false, true}) {
        QJsonObject metrics;
        const auto scenario = single ? "v3-single-file-restore" : "v3-full-restore";
        if (!restore(provider, firstV3, work.filePath(scenario), baseline, single, &metrics, &error)) return fail(error);
        metrics["scenario"] = scenario;
        report(metrics);
    }
    QMap<QString, QByteArray> current;
    for (const auto &source : request.scan.sources) {
        const auto checksum = digest(source.sourcePath);
        if (checksum.isEmpty()) return fail("Changed fixture hashing failed.");
        current.insert(source.restorePath, checksum);
    }
    const QString lastSnapshot = request.remoteSetRoot + "/snapshots/" + request.snapshotId;
    // Recovery must work without settings, journals or the old writer's state.
    // Hide rather than delete it, so cleanup can later reacquire the same state.
    if (!QDir().rename(request.stateDirectory, request.stateDirectory + "-offline")) return fail("Unable to hide local settings.");
    for (const QString &scenario : {QString("old-full-restore"), QString("new-full-restore"), QString("single-file-restore")}) {
        QJsonObject metrics;
        const bool old = scenario == "old-full-restore", single = scenario == "single-file-restore";
        if (!restore(provider, old ? firstSnapshot : lastSnapshot, work.filePath(scenario),
                old ? baseline : current, single, &metrics, &error)) return fail(error);
        metrics["scenario"] = scenario;
        report(metrics);
    }
    if (!QDir().rename(request.stateDirectory + "-offline", request.stateDirectory)) return fail("Unable to re-enable local coordination state.");
    request.retention = 1;
    provider.staging = request.stagingDirectory;
    provider.reset();
    QElapsedTimer cleanupClock;
    cleanupClock.start();
    QJsonObject proposal;
    QString confirmation;
    const bool previewCompleted = IncrementalRepository::cleanup(request, provider, &proposal, &confirmation, &error);
    if (previewCompleted || confirmation.isEmpty() || proposal.isEmpty()
        || error != "Review the exact cleanup proposal and confirm its SHA-256 before deletion."
        || provider.counts.contains("delete_exact") || provider.counts.contains("trash_exact")) return fail("Cleanup preview did not require exact confirmation: " + error);
    request.cleanupConfirmation = confirmation;
    error.clear();
    if (!IncrementalRepository::cleanup(request, provider, &proposal, &confirmation, &error)) return fail(error);
    QVector<RemoteCopy> remaining;
    if (!IncrementalRestore::discover(provider, "repository", &remaining, &error)
        || remaining.size() != 1 || remaining.first().rootPath != lastSnapshot)
        return fail("Cleanup did not leave exactly the intended retained snapshot: " + error);
    auto metrics = provider.metrics();
    metrics["scenario"] = "cleanup-to-one-snapshot";
    metrics["elapsed_ns"] = cleanupClock.nsecsElapsed();
    metrics["remote_tree_bytes"] = treeBytes(provider.root);
    report(metrics);
    // Confirm retained references survived the exact-scope deletion pass.
    provider.staging = temporary;
    if (!restore(provider, lastSnapshot, work.filePath("after-cleanup"), current, false, &metrics, &error)) return fail(error);
    metrics["scenario"] = "retained-restore-after-cleanup";
    report(metrics);
    return 0;
}

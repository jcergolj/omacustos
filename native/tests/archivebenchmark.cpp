#include "archivebenchmark.h"
#include "backupengine.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <cstdio>
#include <zlib.h>

namespace {
qint64 memoryStat(const QByteArray &name)
{
    QFile status("/proc/self/status");
    if (!status.open(QIODevice::ReadOnly)) return -1;
    for (const auto &line : status.readAll().split('\n')) {
        long long kib = 0;
        if (line.startsWith(name) && std::sscanf(line.constData() + name.size(), "%lld kB", &kib) == 1)
            return qint64(kib) * 1024;
    }
    return -1;
}

qint64 memoryPeak() { return memoryStat("VmHWM:"); }
qint64 currentMemory() { return memoryStat("VmRSS:"); }

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

void report(const QJsonObject &value)
{
    qInfo().noquote() << QJsonDocument(value).toJson(QJsonDocument::Compact);
}

// Independently retained remote bytes, not metadata reconstructed from a journal.
class DiskProvider final : public BackupProvider {
public:
    QString root;
    int uploads = 0, inspections = 0, downloads = 0;
    qint64 uploadBytes = 0;
    QString restoreWorkspace;
    qint64 restoreStagedPeak = 0;
    explicit DiskProvider(QString root) : root(std::move(root)) {}
    QString local(const QString &remote) const { return root + remote; }
    bool ensureDirectory(const QString &path, QString *) override { return QDir().mkpath(local(path)); }
    bool upload(const QString &source, const QString &path, QString *) override
    {
        ++uploads;
        uploadBytes += QFileInfo(source).size();
        return QDir().mkpath(QFileInfo(local(path)).absolutePath()) && QFile::copy(source, local(path));
    }
    bool inspect(const QString &path, RemoteFile *file, QString *) override
    {
        ++inspections;
        const QByteArray hash = digest(local(path));
        if (hash.isEmpty()) return false;
        *file = {path, QFileInfo(local(path)).size(), hash};
        return true;
    }
    bool download(const QString &path, const QString &destination, QString *) override
    {
        ++downloads;
        restoreWorkspace = QFileInfo(destination).absolutePath();
        const bool copied = QFile::copy(local(path), destination);
        restoreStagedPeak = qMax(restoreStagedPeak, treeBytes(restoreWorkspace));
        return copied;
    }
    bool list(const QString &, QVector<RemoteItem> *, QString *) override { return false; }
    bool trash(const QString &, QString *) override { return false; }
    bool permanentlyDelete(const QString &, QString *) override { return false; }
};

// Recompress the exact produced tar stream at each level; bounded buffers and
// independent decompressed SHA-256 checks avoid a compression-only microprobe.
bool compressionLevels(const QString &archive, const QString &workspace, const QString &profile)
{
    QByteArray expected;
    for (int level : {1, 3, 6}) {
        const QString output = QDir(workspace).filePath(QString("level-%1.gz").arg(level));
        QElapsedTimer clock;
        clock.start();
        gzFile input = gzopen(QFile::encodeName(archive).constData(), "rb");
        const QByteArray mode = "wb" + QByteArray::number(level);
        gzFile compressed = gzopen(QFile::encodeName(output).constData(), mode.constData());
        if (!input || !compressed) { if (input) gzclose(input); if (compressed) gzclose(compressed); return false; }
        QCryptographicHash original(QCryptographicHash::Sha256);
        char buffer[64 * 1024];
        int count;
        qint64 bytes = 0;
        bool valid = true;
        while ((count = gzread(input, buffer, sizeof(buffer))) > 0) {
            bytes += count;
            original.addData(QByteArrayView(buffer, count));
            if (gzwrite(compressed, buffer, count) != count) { valid = false; break; }
        }
        valid &= count == 0;
        valid &= gzclose(input) == Z_OK;
        valid &= gzclose(compressed) == Z_OK;
        const qint64 elapsed = clock.nsecsElapsed();
        input = gzopen(QFile::encodeName(output).constData(), "rb");
        if (!input) return false;
        QCryptographicHash restored(QCryptographicHash::Sha256);
        while ((count = gzread(input, buffer, sizeof(buffer))) > 0)
            restored.addData(QByteArrayView(buffer, count));
        valid &= count == 0;
        valid &= gzclose(input) == Z_OK;
        if (expected.isEmpty()) expected = original.result();
        if (!valid || original.result() != expected || restored.result() != expected) return false;
        report({{"phase", "gzip-comparison"}, {"profile", profile}, {"level", level},
            {"tar_bytes", bytes}, {"compressed_bytes", QFileInfo(output).size()},
            {"recompression_including_inflate_ms", double(elapsed) / 1000000}, {"round_trip_sha256", QString::fromLatin1(expected.toHex())}});
        if (!QFile::remove(output)) return false;
    }
    return true;
}
}

int compareArchiveBackups(const QStringList &arguments)
{
    const int count = arguments.value(2, "8192").toInt();
    const qint64 bytes = arguments.value(3, "268435456").toLongLong();
    const QString profile = arguments.value(4, "developer");
    if (count <= 0 || bytes < count || (profile != "developer" && profile != "mixed")) return 2;
    QTemporaryDir work(QDir::current().filePath("archive-comparison-XXXXXX"));
    if (!work.isValid() || QStorageInfo(work.path()).fileSystemType() == "tmpfs") return 2;
    const QString sources = work.filePath("sources");
    // Code-shaped records with changing identifiers, plus deterministic binary
    // content in half of mixed files. Identical inputs feed both formats.
    quint32 random = 0x5a17c9e3;
    for (int index = 0; index < count; ++index) {
        const QString path = QDir(sources).filePath(QString("module-%1/file-%2").arg(index / 32).arg(index));
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) return 1;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return 1;
        qint64 remaining = bytes / count + (index < bytes % count ? 1 : 0);
        int line = 0;
        while (remaining > 0) {
            QByteArray chunk;
            if (profile == "mixed" && index % 2) {
                chunk.resize(qMin(remaining, qint64(65536)));
                for (char &byte : chunk) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; byte = char(random); }
            } else {
                chunk = QString("// module %1 record %2\nconst item_%2 = { path: 'src/component/%1', enabled: true };\n").arg(index).arg(line++).toUtf8();
                chunk.truncate(qMin(remaining, qint64(chunk.size())));
            }
            if (file.write(chunk) != chunk.size()) return 1;
            remaining -= chunk.size();
        }
    }
    for (bool archives : {false, true}) {
        const QString format = archives ? "archives" : "individual";
        DiskProvider provider(work.filePath(format + "/remote"));
        BackupEngine engine;
        BackupOptions options;
        options.freshCopy = true;
        options.retainLocalManifest = false;
        options.boundedArchives = archives;
        options.archiveTargetBytes = 16 * 1024 * 1024;
        options.stagingBudget = 64 * 1024 * 1024;
        options.stagingDirectory = work.filePath(format + "/stage");
        options.continuationDirectory = work.filePath(format + "/journal");
        QHash<QString, qint64> timings, phaseMemory, phaseStaging, phaseCurrentMemory;
        qint64 backupCurrentPeak = currentMemory();
        QElapsedTimer clock;
        clock.start();
        qint64 last = 0, stagedPeak = 0, lastResourceSample = -250000000;
        QString phase = "scanning";
        const auto sample = [&](const BackupProgress &progress) {
            const qint64 now = clock.nsecsElapsed();
            timings[phase] += now - last;
            last = now;
            const QString nextPhase = progress.finalizing ? "finalizing" : progress.phase;
            if (progress.finalizing || nextPhase != phase || now - lastResourceSample >= 250000000) {
                const qint64 staged = treeBytes(options.stagingDirectory);
                stagedPeak = qMax(stagedPeak, staged);
                phaseStaging[phase] = qMax(phaseStaging[phase], staged);
                phaseMemory[phase] = qMax(phaseMemory[phase], memoryPeak());
                phaseCurrentMemory[phase] = qMax(phaseCurrentMemory[phase], currentMemory());
                backupCurrentPeak = qMax(backupCurrentPeak, phaseCurrentMemory[phase]);
                lastResourceSample = now;
            }
            phase = nextPhase;
        };
        QString manifest, error;
        BackupResult result;
        const BackupCopyMetadata metadata {"benchmark", "comparison", "Comparison", "copy", QDateTime::currentDateTimeUtc()};
        if (!engine.backup({sources}, "/copy", {}, metadata, provider, &manifest, &error, sample, &result, options)) {
            qCritical().noquote() << error; return 1;
        }
        timings[phase] += clock.nsecsElapsed() - last;
        const double backupMs = double(clock.nsecsElapsed()) / 1000000;
        QVector<BackupEntry> entries;
        BackupManifestInfo info;
        if (!BackupManifest::load(provider.local("/copy/manifest.json"), &entries, &info, &error) || entries.size() != count
            || result.verifiedBytes != bytes || !result.manifestVerified || treeBytes(options.stagingDirectory) != 0) return 1;
        const int backupInspections = provider.inspections;
        const qint64 backupMemory = memoryPeak();
        backupCurrentPeak = qMax(backupCurrentPeak, currentMemory());
        qint64 restoreCurrentPeak = currentMemory();
        clock.restart();
        const auto restored = engine.restoreFiles({entries, work.filePath(format + "/restored"), "/copy"}, provider,
            [&](int restoredCount) {
                if (restoredCount == 1 || restoredCount % 128 == 0 || restoredCount == count)
                    provider.restoreStagedPeak = qMax(provider.restoreStagedPeak, treeBytes(provider.restoreWorkspace));
                restoreCurrentPeak = qMax(restoreCurrentPeak, currentMemory());
            });
        const double restoreMs = double(clock.nsecsElapsed()) / 1000000;
        if (!restored.success || restored.restoredCount != count) { qCritical().noquote() << restored.error; return 1; }
        for (const auto &entry : entries) {
            const QString destination = QDir(work.filePath(format + "/restored")).filePath(entry.restorePath);
            if (QFileInfo(destination).size() != entry.size || digest(destination) != digest(entry.sourcePath)) return 1;
        }
        QJsonObject phases;
        for (auto it = timings.cbegin(); it != timings.cend(); ++it)
            phases.insert(it.key(), QJsonObject {{"elapsed_ms", double(it.value()) / 1000000},
                {"rss_process_high_water_bytes", phaseMemory.value(it.key())}, {"sampled_retained_staging_bytes", phaseStaging.value(it.key())}});
        for (auto it = phaseCurrentMemory.cbegin(); it != phaseCurrentMemory.cend(); ++it) {
            QJsonObject value = phases.value(it.key()).toObject();
            value.insert("rss_sampled_current_peak_bytes", it.value());
            phases.insert(it.key(), value);
        }
        report({{"phase", "comparison"}, {"profile", profile}, {"format", format}, {"logical_files", count},
            {"original_bytes", bytes}, {"backup_ms", backupMs}, {"restore_ms", restoreMs}, {"uploads", provider.uploads},
            {"verification_calls", backupInspections}, {"restore_downloads", provider.downloads},
            {"uploaded_bytes_including_index", provider.uploadBytes}, {"sampled_staging_peak_bytes", stagedPeak},
            {"retained_staging_after_backup_bytes", treeBytes(options.stagingDirectory)},
            {"restore_sampled_staging_peak_bytes", provider.restoreStagedPeak},
            {"backup_rss_process_high_water_bytes", backupMemory},
            {"restore_rss_process_high_water_bytes", memoryPeak()},
            {"backup_rss_sampled_current_peak_bytes", backupCurrentPeak},
            {"restore_rss_sampled_current_peak_bytes", restoreCurrentPeak},
            {"rss_process_high_water_bytes", memoryPeak()}, {"phases", phases}, {"round_trip_validated", true}});
        if (archives) {
            // Use the largest actual tar.gz, not the tiny final partial group.
            BackupArchive largest;
            for (const auto &entry : entries) if (entry.archive.size > largest.size) largest = entry.archive;
            if (!compressionLevels(provider.local(largest.remotePath), work.path(), profile)) return 1;
        }
    }
    return 0;
}

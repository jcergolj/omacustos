#include "backupengine.h"
#include "backupcontinuation.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <cstdio>
#include <unistd.h>

namespace {
qint64 peakRss()
{
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly)) return -1;
    for (const auto &line : status.readAll().split('\n')) {
        if (!line.startsWith("VmHWM:")) continue;
        long long kib = 0;
        if (std::sscanf(line.constData(), "VmHWM: %lld kB", &kib) == 1) return qint64(kib) * 1024;
    }
    return -1;
}

// Read every staged byte, retain only content identities, and expose folder
// metadata at the provider boundary. This measures engine resource behavior
// independently of live Proton latency/storage; it is not a cloud acceptance run.
class MeasuringProvider final : public BackupProvider
{
public:
    QHash<QString, QHash<QString, RemoteFile>> directories;
    qint64 uploadedFiles = 0;
    qint64 uploadedBytes = 0;
    qint64 peakStagedBytes = 0;
    int peakStagedFiles = 0;
    int batches = 0;

    bool ensureDirectory(const QString &, QString *) override { return true; }
    bool supportsDirectoryUpload() const override { return true; }
    bool upload(const QString &local, const QString &remote, QString *) override
    {
        QFile file(local);
        if (!file.open(QIODevice::ReadOnly)) return false;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file)) return false;
        directories[QFileInfo(remote).path()].insert(remote, {remote, file.size(), hash.result()});
        if (QFileInfo(remote).fileName() != "manifest.json") { ++uploadedFiles; uploadedBytes += file.size(); }
        return true;
    }
    bool uploadDirectory(const QString &local, const QString &remote, QString *error) override
    {
        qint64 bytes = 0;
        int files = 0;
        QDirIterator it(local, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            bytes += it.fileInfo().size();
            ++files;
            if (!upload(path, QDir(remote).filePath(QDir(local).relativeFilePath(path)), error)) return false;
        }
        peakStagedBytes = qMax(peakStagedBytes, bytes);
        peakStagedFiles = qMax(peakStagedFiles, files);
        ++batches;
        return true;
    }
    bool inspect(const QString &path, RemoteFile *file, QString *) override
    {
        const auto parent = directories.constFind(QFileInfo(path).path());
        if (parent == directories.cend()) return false;
        const auto entry = parent->constFind(path);
        if (entry == parent->cend()) return false;
        *file = *entry;
        return true;
    }
    bool inspectDirectoryFiles(const QString &path, QVector<RemoteFile> *files, QString *) override
    {
        const auto parent = directories.constFind(path);
        if (parent == directories.cend()) return false;
        *files = parent->values().toVector();
        return true;
    }
    bool download(const QString &, const QString &, QString *) override { return false; }
    bool list(const QString &, QVector<RemoteItem> *, QString *) override { return false; }
    bool trash(const QString &, QString *) override { return false; }
    bool permanentlyDelete(const QString &, QString *) override { return false; }
};
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    const int count = application.arguments().value(1, "877172").toInt();
    const qint64 totalBytes = application.arguments().value(2, "95323650798").toLongLong();
    if (count <= 0 || totalBytes < count) return 2;
    // Keep all benchmark work on the current disk. A temporary directory's RAII
    // cleanup removes only this generated, owned workload.
    QTemporaryDir work(QDir::current().filePath("largebackup-benchmark-XXXXXX"));
    const bool harnessResume = application.arguments().size() > 3;
    const QString workPath = harnessResume ? QFileInfo(application.arguments().at(3)).absoluteFilePath() : work.path();
    const auto filePath = [&](const QString &name) { return QDir(workPath).filePath(name); };
    if (!work.isValid() || QStorageInfo(workPath).fileSystemType() == "tmpfs"
        || (harnessResume && (!workPath.startsWith(QDir::currentPath() + "/largebackup-benchmark-")
            || QFileInfo(workPath).isSymLink()))) return 2;
    const QString sources = filePath("sources");
    const qint64 small = totalBytes / count, largerCount = totalBytes % count;
    QElapsedTimer clock;
    clock.start();
    for (int index = 0; !harnessResume && index < count; ++index) {
        if (index % 10000 == 0) {
            for (int sizeIndex = 0; sizeIndex < 2; ++sizeIndex) {
                QFile seed(filePath(QStringLiteral("seed-%1-%2").arg(index / 10000).arg(sizeIndex)));
                if (!seed.open(QIODevice::WriteOnly) || seed.write(QByteArray(small + sizeIndex, 'a' + sizeIndex)) != small + sizeIndex) return 1;
            }
        }
        const QString parent = QDir(sources).filePath(QStringLiteral("group-%1").arg(index / 1000, 4, 10, QChar('0')));
        if (index % 1000 == 0 && !QDir().mkpath(parent)) return 1;
        const QString path = QDir(parent).filePath(QStringLiteral("file-%1").arg(index, 7, 10, QChar('0')));
        const QByteArray seed = QFile::encodeName(filePath(QStringLiteral("seed-%1-%2").arg(index / 10000).arg(index < largerCount ? 1 : 0)));
        if (::link(seed.constData(), QFile::encodeName(path).constData()) != 0) return 1;
    }
    qInfo().noquote() << QJsonDocument(QJsonObject {{"phase", "generated"}, {"files", count}, {"bytes", totalBytes},
        {"elapsed_ms", clock.elapsed()}, {"rss_peak_bytes", peakRss()}, {"work_directory", workPath},
        {"filesystem", QString::fromLatin1(QStorageInfo(workPath).fileSystemType())}}).toJson(QJsonDocument::Compact);
    MeasuringProvider provider;
    BackupEngine engine;
    BackupOptions options;
    options.freshCopy = true;
    options.stagingDirectory = filePath("staging");
    options.continuationDirectory = filePath("checkpoint");
    bool stop = false;
    options.stopped = [&] { return stop; };
    const BackupCopyMetadata metadata {"benchmark", "representative", "Representative", "same-copy", QDateTime::currentDateTimeUtc()};
    QString manifest, error;
    BackupResult result;
    int reported = harnessResume ? 1 : 0;
    int harnessCheckpointed = 0;
    if (harnessResume) {
        // The scale provider intentionally retains only virtual remote content
        // identities. Rehydrate that fixture from already-observed checkpoints
        // after a harness runtime limit; real subprocess recovery/independent
        // remote damage is tested in workerrecovery-test.
        BackupContinuation observed(options.continuationDirectory);
        if (!observed.open("/copy/same-copy", metadata, &error)) { qCritical().noquote() << error; return 1; }
        for (const auto &entry : observed.verified) {
            provider.directories[QFileInfo(entry.remotePath).path()].insert(entry.remotePath,
                {entry.remotePath, entry.size, entry.checksum});
            ++provider.uploadedFiles;
            provider.uploadedBytes += entry.size;
        }
        harnessCheckpointed = observed.verified.size();
        provider.batches = (harnessCheckpointed + 999) / 1000;
        provider.peakStagedFiles = qMin(harnessCheckpointed, 1000);
        provider.peakStagedBytes = provider.peakStagedFiles * (small + (largerCount > 0 ? 1 : 0));
        qInfo().noquote() << QJsonDocument(QJsonObject {{"phase", "harness-resume"}, {"checkpointed_files", harnessCheckpointed}}).toJson(QJsonDocument::Compact);
    }
    qint64 scanPeak = 0, executionPeak = 0;
    const auto sample = [&](const BackupProgress &progress) {
        if ((progress.phase == "staging" || progress.phase == "checking") && progress.processedFiles == 0)
            scanPeak = qMax(scanPeak, peakRss());
        if (!progress.finalizing) executionPeak = qMax(executionPeak, peakRss());
        if (progress.phase == "preparing" && progress.verifiedFiles >= qMin(count / 2, 10000) && reported == 0) stop = true;
        if (progress.verifiedFiles / 100000 > reported) {
            reported = progress.verifiedFiles / 100000;
            qInfo().noquote() << QJsonDocument(QJsonObject {{"phase", "checkpointed"}, {"verified_files", progress.verifiedFiles},
                {"rss_peak_bytes", peakRss()}, {"elapsed_ms", clock.elapsed()}}).toJson(QJsonDocument::Compact);
        }
    };
    int checkpointed = 10000;
    if (!harnessResume) {
        const bool first = engine.backup({sources}, "/copy/same-copy", {}, metadata, provider, &manifest, &error, sample, &result, options);
        if (first || !result.interrupted || result.verifiedFiles <= 0 || !manifest.isEmpty()) {
            qCritical().noquote() << "Expected an interrupted checkpointed attempt:" << error;
            return 1;
        }
        checkpointed = result.verifiedFiles;
        qInfo().noquote() << QJsonDocument(QJsonObject {{"phase", "interrupted"}, {"verified_files", checkpointed}, {"rss_peak_bytes", peakRss()}}).toJson(QJsonDocument::Compact);
    }
    stop = false;
    reported = 1; // Resume without interrupting again.
    if (!engine.backup({sources}, "/copy/same-copy", {}, metadata, provider, &manifest, &error, sample, &result, options)) {
        qCritical().noquote() << error;
        return 1;
    }
    if (result.verifiedFiles != count || result.verifiedBytes != totalBytes || !result.manifestVerified
        || provider.uploadedFiles != count || provider.uploadedBytes != totalBytes || provider.peakStagedFiles > 1000
        || provider.peakStagedBytes > options.stagingBudget) return 1;
    qInfo().noquote() << QJsonDocument(QJsonObject {{"phase", "complete"}, {"verified_files", result.verifiedFiles},
        {"verified_bytes", result.verifiedBytes}, {"uploaded_files", provider.uploadedFiles}, {"checkpointed_before_interruption", checkpointed},
        {"peak_payload_staging_bytes", provider.peakStagedBytes}, {"peak_batch_files", provider.peakStagedFiles}, {"batches", provider.batches},
        {"selection_rss_high_water_bytes", scanPeak}, {"execution_rss_high_water_bytes", executionPeak}, {"final_rss_high_water_bytes", peakRss()},
        {"harness_resume_verified_files", harnessCheckpointed}, {"manifest_bytes", QFileInfo(manifest).size()}, {"elapsed_ms", clock.elapsed()}}).toJson(QJsonDocument::Compact);
    if (harnessResume) QDir(workPath).removeRecursively();
    return 0;
}

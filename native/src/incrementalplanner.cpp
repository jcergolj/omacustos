#include "incrementalplanner.h"
#include "incrementalmanifest.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// Walk from / with openat so neither the leaf nor a replaced parent symlink is
// followed. O_NONBLOCK prevents a replaced FIFO from hanging before fstat.
int openSource(const QString &path)
{
    int descriptor = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const QStringList parts = path.mid(1).split('/');
    for (qsizetype index = 0; descriptor >= 0 && index < parts.size(); ++index) {
        const int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK
            | (index + 1 < parts.size() ? O_DIRECTORY : 0);
        const int next = ::openat(descriptor, QFile::encodeName(parts[index]).constData(), flags);
        const int savedError = errno;
        ::close(descriptor);
        descriptor = next;
        errno = savedError;
    }
    return descriptor;
}

bool sameCapture(const struct stat &left, const struct stat &right)
{
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino
        && left.st_size == right.st_size && left.st_mode == right.st_mode
        && left.st_mtim.tv_sec == right.st_mtim.tv_sec && left.st_mtim.tv_nsec == right.st_mtim.tv_nsec
        && left.st_ctim.tv_sec == right.st_ctim.tv_sec && left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

bool capture(const IncrementalSource &source, const std::function<bool()> &stopped,
    IncrementalPlannedFile *file, QString *error, QFile *output = nullptr)
{
    const auto fail = [&](const QString &reason) { *error = reason; return false; };
    if (stopped && stopped()) return fail("Planning was stopped.");
    const int descriptor = openSource(source.sourcePath);
    if (descriptor < 0) return fail(QString::fromLocal8Bit(std::strerror(errno)));
    QFile input;
    if (!input.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        return fail(input.errorString());
    }
    struct stat before {}, after {}, current {};
    if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode))
        return fail("The selected source is not a readable regular file.");
    constexpr qint64 billion = 1000000000;
    const qint64 seconds = before.st_mtim.tv_sec;
    if (before.st_size < 0 || before.st_size > 9007199254740991LL || seconds < 0
        || seconds > (std::numeric_limits<qint64>::max() - before.st_mtim.tv_nsec) / billion)
        return fail("Source size or modification time is outside the v4 metadata range.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 bytes = 0;
    while (true) {
        if (stopped && stopped()) return fail("Planning was stopped.");
        const QByteArray chunk = input.read(64 * 1024);
        if (input.error() != QFileDevice::NoError) return fail(input.errorString());
        if (chunk.isEmpty()) break;
        bytes += chunk.size();
        if (bytes > before.st_size) return fail("Source changed while hashing.");
        if (output && output->write(chunk) != chunk.size())
            return fail("Waiting for staging storage: " + output->errorString());
        hash.addData(chunk);
    }
    if (::fstat(descriptor, &after) != 0) return fail("Unable to recheck the captured source.");
    const int currentDescriptor = openSource(source.sourcePath);
    if (currentDescriptor < 0) return fail("Source path changed while hashing.");
    const bool stable = ::fstat(currentDescriptor, &current) == 0 && sameCapture(before, after)
        && sameCapture(before, current) && bytes == before.st_size;
    ::close(currentDescriptor);
    if (!stable) return fail("Source changed while hashing.");
    file->sourcePath = source.sourcePath;
    file->restorePath = source.restorePath;
    file->size = bytes;
    file->checksum = hash.result();
    file->mode = before.st_mode & 0777;
    file->mtimeNs = QString::number(seconds * billion + before.st_mtim.tv_nsec);
    return true;
}

QString contentKey(qint64 size, const QByteArray &checksum)
{
    return QString::number(size) + ':' + QString::fromLatin1(checksum.toHex());
}

bool sameSavedFile(const IncrementalPlannedFile &file, const QJsonObject &entry)
{
    return file.size == entry.value("size").toInteger()
        && file.checksum.toHex() == entry.value("sha256").toString().toLatin1()
        && file.mode == entry.value("mode").toInt() && file.mtimeNs == entry.value("mtime_ns").toString();
}

} // namespace

bool IncrementalPlanner::stage(const IncrementalPlannedFile &expected, const QString &outputPath,
    QString *error, const std::function<bool()> &stopped)
{
    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        if (error) *error = "Waiting for staging storage: " + output.errorString();
        return false;
    }
    IncrementalPlannedFile observed;
    QString reason;
    const bool captured = capture({expected.sourcePath, expected.restorePath}, stopped, &observed, &reason, &output);
    const bool matches = captured && observed.size == expected.size && observed.checksum == expected.checksum
        && observed.mode == expected.mode && observed.mtimeNs == expected.mtimeNs;
    const bool flushed = matches && output.flush();
    output.close();
    if (flushed && output.setPermissions(QFileDevice::ReadOwner)) return true;
    QFile::remove(outputPath);
    if (error) *error = !reason.isEmpty() ? reason : !matches ? "Source changed since planning; replan before upload."
        : "Waiting for staging storage: unable to flush or protect captured bytes.";
    return false;
}

IncrementalPlan IncrementalPlanner::plan(const IncrementalPlanRequest &request)
{
    IncrementalPlan result;
    result.lastSuccessfulCheck = request.lastSuccessfulCheck;
    const auto fail = [&](const QString &path, const QString &reason) {
        result.files.clear();
        result.removed.clear();
        result.requiredArchives = {};
        result.snapshotRequired = false;
        result.issues.append({path, "planning", reason});
        return result;
    };
    if (!request.scanIssues.isEmpty()) {
        result.issues = request.scanIssues;
        return result;
    }
    if (!IncrementalManifest::validId(request.setId) || !request.checkedAt.isValid()
        || (request.lastSuccessfulCheck.isValid() && request.checkedAt < request.lastSuccessfulCheck))
        return fail({}, "Invalid set identity or check time.");

    QJsonObject previous;
    if (!request.previousManifest.isEmpty()) {
        QString error;
        if (!IncrementalManifest::validate(request.previousManifest, request.setId, request.previousSnapshotId, &error))
            return fail({}, error);
        previous = QJsonDocument::fromJson(request.previousManifest).object();
        result.lastSavedChange = QDateTime::fromString(previous.value("created_at").toString(), Qt::ISODateWithMs);
        if (request.checkedAt < result.lastSavedChange) return fail({}, "Check time predates the saved snapshot.");
    } else if (!request.previousSnapshotId.isEmpty()) {
        return fail({}, "A previous snapshot identity requires its complete manifest.");
    }

    QVector<IncrementalSource> sources = request.sources;
    std::sort(sources.begin(), sources.end(), [](const auto &left, const auto &right) {
        if (left.restorePath != right.restorePath) return left.restorePath < right.restorePath;
        return left.sourcePath < right.sourcePath;
    });
    QSet<QString> destinations;
    for (const auto &source : sources) {
        if (!IncrementalManifest::safeRelativePath(source.restorePath) || destinations.contains(source.restorePath)
            || !source.sourcePath.startsWith('/') || QDir::cleanPath(source.sourcePath) != source.sourcePath
            || source.sourcePath.contains(QChar::Null) || source.sourcePath == "/")
            return fail(source.sourcePath, "Unsafe or duplicate source/restore mapping.");
        destinations.insert(source.restorePath);
    }
    for (const auto &source : sources) {
        QString parent = source.restorePath;
        while (parent.contains('/')) {
            parent.truncate(parent.lastIndexOf('/'));
            if (destinations.contains(parent)) return fail(source.sourcePath, "Restore file/directory conflict.");
        }
    }

    QHash<QString, QJsonObject> oldPaths, content;
    for (const auto &value : previous.value("entries").toArray()) {
        const QJsonObject entry = value.toObject();
        oldPaths.insert(entry.value("restore").toString(), entry);
        const QString key = contentKey(entry.value("size").toInteger(), QByteArray::fromHex(entry.value("sha256").toString().toLatin1()));
        const QJsonObject existing = content.value(key);
        const QString archive = entry.value("archive").toString();
        if (existing.isEmpty() || archive < existing.value("archive").toString()
            || (archive == existing.value("archive").toString() && entry.value("member").toString() < existing.value("member").toString()))
            content.insert(key, entry);
    }

    QSet<QString> required;
    bool changed = previous.isEmpty();
    for (const auto &source : sources) {
        IncrementalPlannedFile file;
        QString error;
        if (!capture(source, request.stopped, &file, &error)) return fail(source.sourcePath, error);
        const auto old = oldPaths.constFind(source.restorePath);
        file.change = old == oldPaths.cend() ? IncrementalPlannedFile::Change::Added
            : sameSavedFile(file, old.value()) ? IncrementalPlannedFile::Change::Reused : IncrementalPlannedFile::Change::Changed;
        changed = changed || file.change != IncrementalPlannedFile::Change::Reused;
        const auto match = content.constFind(contentKey(file.size, file.checksum));
        if (match != content.cend()) {
            file.archiveId = match->value("archive").toString();
            file.memberPath = match->value("member").toString();
            required.insert(file.archiveId);
        }
        result.files.append(file);
    }
    // Empty selections still honor cancellation and cannot advance check history.
    if (request.stopped && request.stopped()) return fail({}, "Planning was stopped.");
    for (auto entry = oldPaths.cbegin(); entry != oldPaths.cend(); ++entry)
        if (!destinations.contains(entry.key())) result.removed.append(entry.key());
    result.removed.sort();
    QVector<QJsonObject> archives;
    for (const auto &value : previous.value("archives").toArray())
        if (required.contains(value.toObject().value("id").toString())) archives.append(value.toObject());
    std::sort(archives.begin(), archives.end(), [](const auto &left, const auto &right) {
        return left.value("id").toString() < right.value("id").toString();
    });
    for (const auto &archive : archives) result.requiredArchives.append(archive);
    result.snapshotRequired = changed || !result.removed.isEmpty();
    result.lastSuccessfulCheck = request.checkedAt.toUTC();
    result.success = true;
    return result;
}

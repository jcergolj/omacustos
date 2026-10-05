#include "backupstaging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUuid>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
bool within(const QString &path, const QString &root)
{
    return path == root || path.startsWith(root == "/" ? root : root + '/');
}

// Resolve existing ancestors too, so a not-yet-created child under a symlink
// cannot bypass source overlap checks.
QString resolved(QString path)
{
    path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QStringList suffix;
    while (!QFileInfo::exists(path) && path != "/") {
        suffix.prepend(QFileInfo(path).fileName());
        path = QFileInfo(path).absolutePath();
    }
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QString() : QDir(canonical).filePath(suffix.join('/'));
}

bool owned(const QString &path)
{
    QFile marker(QDir(path).filePath(".omacustos-owner"));
#ifdef Q_OS_UNIX
    if (QFileInfo(path).ownerId() != ::getuid() || QFileInfo(marker.fileName()).ownerId() != ::getuid()) return false;
#endif
    return !QFileInfo(path).isSymLink() && !QFileInfo(marker.fileName()).isSymLink()
        && marker.open(QIODevice::ReadOnly) && marker.readAll() == QByteArray("omacustos-staging-v1\n");
}
}

BackupStaging::~BackupStaging()
{
    if (!workspace.isEmpty() && owned(workspace)) QDir(workspace).removeRecursively();
}

bool BackupStaging::validate(const QString &directory, const QStringList &sources, QString *error)
{
    const auto fail = [&](const QString &message) { if (error) *error = message; return false; };
    if (directory.trimmed().isEmpty() || !QDir::isAbsolutePath(directory))
        return fail(QStringLiteral("Choose an absolute staging directory on a writable disk."));
    const QString destination = resolved(directory);
    if (destination.isEmpty() || QFileInfo(directory).isSymLink())
        return fail(QStringLiteral("The staging directory cannot be resolved safely."));
    for (const QString &source : sources) {
        const QString root = resolved(source);
        if (!root.isEmpty() && (within(destination, root) || within(root, destination)))
            return fail(QStringLiteral("The staging directory must not overlap a selected source: %1").arg(source));
    }
    QString existing = destination;
    while (!QFileInfo::exists(existing) && existing != "/") existing = QFileInfo(existing).absolutePath();
    const QStorageInfo storage(existing);
    if (!storage.isValid() || !storage.isReady() || storage.isReadOnly()
        || storage.fileSystemType() == "tmpfs" || storage.fileSystemType() == "ramfs")
        return fail(QStringLiteral("Staging requires writable disk-backed storage; RAM-backed temporary storage is not supported."));
    if (QFileInfo::exists(directory) && (!QFileInfo(directory).isDir() || !QFileInfo(directory).isWritable()))
        return fail(QStringLiteral("The staging location is not a writable directory."));
    return true;
}

bool BackupStaging::open(const QString &configuredDirectory, const QStringList &sources, QString *error)
{
    QStringList candidates;
    if (!configuredDirectory.isEmpty()) candidates.append(configuredDirectory);
    else {
        candidates.append(QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath("omacustos/staging"));
#ifdef Q_OS_UNIX
        candidates.append(QStringLiteral("/var/tmp/omacustos-%1/staging").arg(::getuid()));
#endif
    }
    for (const QString &candidate : candidates) {
        const bool existed = QFileInfo::exists(candidate);
        if (!validate(candidate, sources, error) || !QDir().mkpath(candidate)) continue;
        // A chosen disk/folder may belong to the user or be shared. Never change
        // its permissions; only newly created application directories are ours.
        if (!existed && !QFile::setPermissions(candidate, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) continue;
        base = QFileInfo(candidate).canonicalFilePath();
        break;
    }
    if (base.isEmpty()) {
        if (error && error->isEmpty()) *error = QStringLiteral("Unable to create disk-backed staging. Choose another staging disk in advanced settings.");
        return false;
    }
    // Serialize creation/recovery across GUI and workers using this staging disk.
    QLockFile recovery(QDir(base).filePath(".recovery.lock"));
    recovery.setStaleLockTime(0);
    if (!recovery.tryLock(5000)) {
        if (error) *error = QStringLiteral("Staging recovery is busy; retry shortly.");
        return false;
    }
    for (const QString &name : QDir(base).entryList({"attempt-*"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString stale = QDir(base).filePath(name);
        if (!owned(stale)) continue;
        QLockFile active(stale + ".lock");
        active.setStaleLockTime(0);
        if (active.tryLock(0)) QDir(stale).removeRecursively();
    }
    workspace = QDir(base).filePath("attempt-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    lock = std::make_unique<QLockFile>(workspace + ".lock");
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0) || !QDir().mkdir(workspace)) {
        if (error) *error = QStringLiteral("Unable to create a private backup staging workspace.");
        return false;
    }
    if (!QFile::setPermissions(workspace, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        QDir().rmdir(workspace);
        workspace.clear();
        if (error) *error = QStringLiteral("Unable to protect the private staging workspace.");
        return false;
    }
    QFile marker(QDir(workspace).filePath(".omacustos-owner"));
    if (!marker.open(QIODevice::WriteOnly) || marker.write("omacustos-staging-v1\n") != 21 || !marker.flush()) {
        if (error) *error = QStringLiteral("Unable to record staging ownership.");
        return false;
    }
#ifdef Q_OS_UNIX
    bool durable = ::fsync(marker.handle()) == 0;
    for (const QString &path : {workspace, base}) {
        const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY);
        durable = fd >= 0 && ::fsync(fd) == 0 && durable;
        if (fd >= 0) ::close(fd);
    }
    if (!durable) {
        if (error) *error = QStringLiteral("Unable to durably record staging ownership.");
        return false;
    }
#endif
    if (error) error->clear();
    return true;
}

bool BackupStaging::reset(QString *error)
{
    const QString payloads = QDir(workspace).filePath("payloads");
    if (!owned(workspace) || (QFileInfo::exists(payloads) && !QDir(payloads).removeRecursively())) {
        if (error) *error = QStringLiteral("Unable to release owned staging payloads: %1").arg(payloads);
        return false;
    }
    return true;
}

bool BackupStaging::hasSpace(qint64 bytes, QString *error) const
{
    const QStorageInfo storage(base);
    // Leave room for filesystem metadata and the final manifest. Quotas are
    // checked by actual writes; bytesAvailable is only a preflight hint.
    constexpr qint64 reserve = 16 * 1024 * 1024;
    if (bytes >= 0 && storage.isReady() && storage.bytesAvailable() >= reserve
        && storage.bytesAvailable() - reserve >= bytes) {
        if (error) error->clear();
        return true;
    }
    if (error) *error = QStringLiteral("Waiting for staging space: %1 bytes required plus %2 bytes reserve on %3; %4 bytes available. Free space or choose another staging disk in advanced settings.")
        .arg(bytes).arg(reserve).arg(base).arg(qMax(qint64(0), storage.bytesAvailable()));
    return false;
}

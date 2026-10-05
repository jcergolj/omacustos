#include "localprovider.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

LocalProvider::LocalProvider(QString rootPath)
    : rootPath(std::move(rootPath))
{
}

static bool validRemotePath(const QString &remotePath)
{
    const QString normalized = QDir::cleanPath(remotePath);

    return !remotePath.startsWith('/') && !normalized.isEmpty() && normalized != QStringLiteral(".")
        && !remotePath.split('/').contains(QStringLiteral(".."))
        && normalized != QStringLiteral("..") && !normalized.startsWith(QStringLiteral("../"))
        && !normalized.contains(QStringLiteral("/../"));
}

static bool safeLocalPath(const QString &rootPath, const QString &remotePath)
{
    const QString root = QFileInfo(rootPath).canonicalFilePath();
    if (root.isEmpty()) {
        return false;
    }

    QString current = root;
    const QStringList parts = QDir::cleanPath(remotePath).split('/', Qt::SkipEmptyParts);
    for (int index = 0; index < parts.size(); ++index) {
        current = QDir(current).filePath(parts.at(index));
        const QFileInfo component(current);
        if (component.isSymLink()) {
            return false;
        }
        if (!component.exists()) {
            break;
        }
        if (index + 1 < parts.size() && !component.isDir()) {
            return false;
        }
    }
    return true;
}

static QString trashPath(const QString &rootPath, const QString &remotePath)
{
    return QDir(rootPath).filePath(QDir(QStringLiteral(".trash")).filePath(remotePath));
}

bool LocalProvider::upload(const QString &localPath, const QString &remotePath, QString *error)
{
    if (!validRemotePath(remotePath) || !safeLocalPath(rootPath, remotePath)) {
        if (error != nullptr) {
            *error = QStringLiteral("The provider path is invalid.");
        }

        return false;
    }

    const QString destination = QDir(rootPath).filePath(remotePath);
    QDir().mkpath(QFileInfo(destination).absolutePath());

    if (QFile::exists(destination)) {
        QFile::remove(destination);
    }

    if (QFile::copy(localPath, destination)) {
        return true;
    }

    if (error != nullptr) {
        *error = QStringLiteral("Unable to copy the file to the provider.");
    }

    return false;
}

bool LocalProvider::ensureDirectory(const QString &remotePath, QString *error)
{
    if (!validRemotePath(remotePath) || !safeLocalPath(rootPath, remotePath)) {
        if (error != nullptr) {
            *error = QStringLiteral("The provider path is invalid.");
        }
        return false;
    }
    if (!QDir().mkpath(QDir(rootPath).filePath(remotePath))) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to create the provider folder.");
        }
        return false;
    }
    return true;
}

bool LocalProvider::download(const QString &remotePath, const QString &localPath, QString *error)
{
    if (!validRemotePath(remotePath) || !safeLocalPath(rootPath, remotePath)) {
        if (error != nullptr) {
            *error = QStringLiteral("The provider path is invalid.");
        }

        return false;
    }

    if (QFile::exists(localPath)) {
        QFile::remove(localPath);
    }

    if (QFile::copy(QDir(rootPath).filePath(remotePath), localPath)) {
        return true;
    }

    if (error != nullptr) {
        *error = QStringLiteral("Unable to download the file from the provider.");
    }

    return false;
}

bool LocalProvider::inspect(const QString &remotePath, RemoteFile *file, QString *error)
{
    if (!validRemotePath(remotePath) || !safeLocalPath(rootPath, remotePath)) {
        if (error != nullptr) {
            *error = QStringLiteral("The provider path is invalid.");
        }

        return false;
    }

    if (file == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("A destination for remote file metadata is required.");
        }

        return false;
    }

    const QFileInfo info(QDir(rootPath).filePath(remotePath));

    if (!info.isFile()) {
        if (error != nullptr) {
            *error = QStringLiteral("The remote file is unavailable.");
        }

        return false;
    }

    QFile source(info.filePath());
    if (!source.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("The remote file could not be read.");
        }

        return false;
    }

    file->path = remotePath;
    file->size = info.size();
    file->checksum = QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256);

    return true;
}

bool LocalProvider::list(const QString &remotePath, QVector<RemoteItem> *items, QString *error)
{
    if (items == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("A destination for remote items is required.");
        }
        return false;
    }
    items->clear();
    if (!validRemotePath(remotePath) || !safeLocalPath(rootPath, remotePath)) {
        if (error != nullptr) {
            *error = QStringLiteral("The provider path is invalid.");
        }
        return false;
    }

    const QString directoryPath = QDir(rootPath).filePath(remotePath);
    const QFileInfo directory(directoryPath);
    if (!directory.isDir()) {
        if (error != nullptr) {
            *error = QStringLiteral("The remote folder is unavailable.");
        }
        return false;
    }

    const QDir directoryContents(directoryPath);
    for (const QFileInfo &info : directoryContents.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name)) {
        if (info.fileName() == QStringLiteral(".trash") || info.isSymLink()) {
            continue;
        }
        items->append({
            QDir::cleanPath(QDir(remotePath).filePath(info.fileName())),
            info.fileName(),
            info.isDir(),
            info.isFile() ? info.size() : 0,
            info.lastModified(),
        });
    }
    return true;
}

bool LocalProvider::trash(const QString &remotePath, QString *error)
{
    if (!validRemotePath(remotePath) || !safeLocalPath(rootPath, remotePath)) {
        if (error != nullptr) {
            *error = QStringLiteral("The provider path is invalid.");
        }
        return false;
    }

    const QString source = QDir(rootPath).filePath(remotePath);
    if (!QFileInfo::exists(source)) {
        return true;
    }
    const QString destination = trashPath(rootPath, remotePath);
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to create the provider trash folder.");
        }
        return false;
    }
    if (QFileInfo::exists(destination)) {
        QDir(destination).removeRecursively();
    }
    if (!QFile::rename(source, destination)) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to move the remote copy to trash.");
        }
        return false;
    }
    return true;
}

bool LocalProvider::permanentlyDelete(const QString &remotePath, QString *error)
{
    if (!validRemotePath(remotePath) || !safeLocalPath(rootPath, remotePath)) {
        if (error != nullptr) {
            *error = QStringLiteral("The provider path is invalid.");
        }
        return false;
    }

    const QString trashed = trashPath(rootPath, remotePath);
    if (QFileInfo(trashed).isDir()) {
        if (!QDir(trashed).removeRecursively()) {
            if (error != nullptr) {
                *error = QStringLiteral("Unable to permanently delete the trashed remote copy.");
            }
            return false;
        }
        return true;
    }
    if (QFileInfo(trashed).exists() && !QFile::remove(trashed)) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to permanently delete the trashed remote copy.");
        }
        return false;
    }
    return true;
}

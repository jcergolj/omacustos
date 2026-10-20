#include "protonprovider.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QtMath>

#include <optional>
#include <limits>

namespace {

bool contentMetadata(const QJsonObject &object, const QString &path, RemoteFile *file)
{
    const QJsonValue size = object.contains(QStringLiteral("size"))
        ? object.value(QStringLiteral("size"))
        : object.value(QStringLiteral("activeRevision")).toObject().value(QStringLiteral("claimedSize"));
    const QString hex = object.value(QStringLiteral("sha256")).toString();
    const QByteArray checksum = QByteArray::fromHex(hex.toLatin1());
    if ((object.contains(QStringLiteral("sha256")) && !object.value(QStringLiteral("sha256")).isString())
        || !size.isDouble() || !qIsFinite(size.toDouble()) || size.toDouble() < 0
        || size.toDouble() >= static_cast<double>(std::numeric_limits<qint64>::max())
        || size.toDouble() != qFloor(size.toDouble())
        || (!hex.isEmpty() && (checksum.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256)
            || QString::fromLatin1(checksum.toHex()) != hex.toLower()))) {
        return false;
    }
    *file = {path, static_cast<qint64>(size.toDouble()), checksum};
    return true;
}

QString itemName(const QJsonObject &object)
{
    const QJsonValue name = object.value(QStringLiteral("name"));
    return name.isObject() ? name.toObject().value(QStringLiteral("value")).toString() : name.toString();
}

}

ProtonProvider::ProtonProvider(ProcessRunner &runner)
    : runner(runner)
{
}

void ProtonProvider::beginBackupOperation()
{
    ensuredDirectories.clear();
    backupOperation = true;
}

void ProtonProvider::endBackupOperation()
{
    backupOperation = false;
    ensuredDirectories.clear();
}

bool ProtonProvider::upload(const QString &localPath, const QString &remotePath, QString *error)
{
    const QString remoteName = QFileInfo(remotePath).fileName();
    if (!remotePath.startsWith('/') || remoteName.isEmpty() || remoteName == QStringLiteral(".")
        || remotePath.split('/').contains(QStringLiteral(".."))) {
        if (error != nullptr) {
            *error = QStringLiteral("The provider upload path is invalid.");
        }
        return false;
    }

    QString uploadPath = localPath;
    std::optional<QTemporaryDir> staging;
    if (QFileInfo(localPath).fileName() != remoteName) {
        staging.emplace(QDir::temp().filePath(QStringLiteral("omacustos-upload-XXXXXX")));
        if (!staging->isValid()) {
            if (error != nullptr) {
                *error = QStringLiteral("The Proton Drive upload staging folder could not be created.");
            }
            return false;
        }
        // The CLI retains local basenames; stage privately rather than renaming
        // the source or using an intermediate remote name that could collide.
        uploadPath = staging->filePath(remoteName);
        QFile source(localPath);
        if (!source.copy(uploadPath)) {
            if (error != nullptr) {
                *error = QStringLiteral("The file could not be staged for Proton Drive upload: %1").arg(source.errorString());
            }
            return false;
        }
    }

    const bool uploaded = run({
        QStringLiteral("filesystem"), QStringLiteral("upload"), QStringLiteral("-j"),
        QStringLiteral("-f"), QStringLiteral("replace"), QStringLiteral("-d"), QStringLiteral("replace"),
        QStringLiteral("-t"), uploadPath, QFileInfo(remotePath).path(),
    }, error);
    // The engine retries after ensuring the parent again. Any ancestor may have
    // disappeared, so that recheck must not trust this operation's cache.
    if (!uploaded) ensuredDirectories.clear();
    return uploaded;
}

bool ProtonProvider::uploadDirectory(const QString &localPath, const QString &remotePath, QString *error)
{
    const QFileInfo source(localPath);
    const QString remoteName = QFileInfo(remotePath).fileName();
    if (!remotePath.startsWith('/') || remotePath != QDir::cleanPath(remotePath)
        || remoteName.isEmpty() || remotePath.split('/').contains(QStringLiteral(".."))
        || !source.isDir() || source.isSymLink() || source.fileName() != remoteName) {
        if (error != nullptr) {
            *error = QStringLiteral("The provider folder upload path is invalid.");
        }
        return false;
    }

    // Preserve the prepared tree's paths. Merge allows a whole-folder retry to
    // reuse successfully transferred children without trashing the copy folder.
    const bool uploaded = run({
        QStringLiteral("filesystem"), QStringLiteral("upload"), QStringLiteral("-j"),
        QStringLiteral("-f"), QStringLiteral("replace"), QStringLiteral("-d"), QStringLiteral("merge"),
        QStringLiteral("-t"), source.absoluteFilePath(), QFileInfo(remotePath).path(),
    }, error);
    if (!uploaded) ensuredDirectories.clear();
    return uploaded;
}

bool ProtonProvider::uploadImmutable(const QString &localPath, const QString &remotePath, QString *error)
{
    if (!remotePath.startsWith('/') || remotePath != QDir::cleanPath(remotePath)
        || remotePath.split('/').contains("..") || remotePath.contains(QChar::Null) || remotePath.contains('\\')
        || QFileInfo(localPath).fileName() != QFileInfo(remotePath).fileName()) {
        if (error) *error = "Immutable uploads require an exact staged basename.";
        return false;
    }
    // In JSON mode unresolved conflicts fail, rather than prompting. Do not use
    // replace, rename or create-new-revision. An identical-content skip is safe
    // only after the caller's mandatory full readback verification.
    return run({"filesystem", "upload", "-j", "-t", localPath, QFileInfo(remotePath).path()}, error);
}

RemoteLookup ProtonProvider::lookup(const QString &remotePath, QString *error)
{
    if (!remotePath.startsWith('/') || remotePath != QDir::cleanPath(remotePath)
        || remotePath.split('/').contains("..") || remotePath.contains(QChar::Null) || remotePath.contains('\\')) {
        if (error) *error = "Unsafe provider lookup path.";
        return RemoteLookup::Unknown;
    }
    const QString parent = QFileInfo(remotePath).path();
    QJsonArray values;
    if (!listing(parent, &values, error)) return RemoteLookup::Unknown;
    QSet<QString> seen;
    bool found = false;
    for (const auto &value : values) {
        const auto object = value.toObject();
        const QString name = itemName(object);
        const QString path = object.value("path").toString(QDir(parent).filePath(name));
        if (!value.isObject() || name.isEmpty() || name == "." || name == ".." || name.contains('/')
            || name.contains(QChar::Null) || path != QDir(parent).filePath(name) || seen.contains(name)) {
            if (error) *error = "Ambiguous or malformed Proton Drive listing; absence is unknown.";
            return RemoteLookup::Unknown;
        }
        seen.insert(name);
        found = found || path == remotePath;
    }
    return found ? RemoteLookup::Found : RemoteLookup::Missing;
}

bool ProtonProvider::ensureDirectory(const QString &remotePath, QString *error)
{
    if (remotePath.isEmpty() || !remotePath.startsWith('/')) {
        if (error != nullptr) {
            *error = QStringLiteral("The provider path is invalid.");
        }
        return false;
    }
    const QStringList parts = remotePath.split('/', Qt::SkipEmptyParts);
    QString current = QStringLiteral("/");
    for (const QString &part : parts) {
        if (current != QStringLiteral("/")) {
            current += QStringLiteral("/");
        }
        current += part;
        if (backupOperation && ensuredDirectories.contains(current)) continue;
        QVector<RemoteItem> children;
        QString listError;
        if (list(current, &children, &listError)) {
            if (backupOperation) ensuredDirectories.insert(current);
            continue;
        }
        if (!run({QStringLiteral("filesystem"), QStringLiteral("create-folder"), QFileInfo(current).path(), part}, error)) {
            ensuredDirectories.clear();
            return false;
        }
        if (backupOperation) ensuredDirectories.insert(current);
    }
    return true;
}

bool ProtonProvider::download(const QString &remotePath, const QString &localPath, QString *error)
{
    const QString localFolder = QFileInfo(localPath).absolutePath();
    if (!QDir().mkpath(localFolder)) {
        if (error != nullptr) {
            *error = QStringLiteral("The local download folder could not be created.");
        }
        return false;
    }
    // CLI downloads preserve remote basenames and can remove conflicting files
    // or folders. Keep those effects inside a private directory.
    QTemporaryDir staging(QDir(localFolder).filePath(QStringLiteral(".omacustos-download-XXXXXX")));
    if (!staging.isValid()) {
        if (error != nullptr) {
            *error = QStringLiteral("The Proton Drive download staging folder could not be created.");
        }
        return false;
    }
    if (!run({
        QStringLiteral("filesystem"), QStringLiteral("download"), QStringLiteral("-j"),
        QStringLiteral("-f"), QStringLiteral("remove"), QStringLiteral("-d"), QStringLiteral("remove"),
        remotePath, staging.path(),
    }, error)) {
        return false;
    }

    QFile downloaded(staging.filePath(QFileInfo(remotePath).fileName()));
    // Engine downloads target an unused path inside private staging on this
    // filesystem. Move those bytes instead of copying them into a second file.
    // QFile::rename never overwrites an existing destination; those callers
    // retain the atomic replacement path below.
    if (!QFileInfo::exists(localPath) && !QFileInfo(localPath).isSymLink()
        && downloaded.rename(localPath)) {
        return true;
    }
    QSaveFile destination(localPath);
    destination.setDirectWriteFallback(false);
    const auto placementFailure = [&] {
        if (error != nullptr) {
            *error = QStringLiteral("The downloaded Proton Drive file could not be placed at the requested path.");
        }
        return false;
    };
    if (!downloaded.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) {
        return placementFailure();
    }
    while (!downloaded.atEnd()) {
        const QByteArray bytes = downloaded.read(1024 * 1024);
        if (bytes.isEmpty() || destination.write(bytes) != bytes.size()) return placementFailure();
    }
    return destination.commit() || placementFailure();
}

bool ProtonProvider::inspect(const QString &remotePath, RemoteFile *file, QString *error)
{
    if (file == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("A destination for remote file metadata is required.");
        }

        return false;
    }

    const ProcessOutput output = runner.run({QStringLiteral("filesystem"), QStringLiteral("info"), QStringLiteral("-j"), remotePath});
    if (!output.successful()) {
        if (error != nullptr) {
            *error = output.standardError.isEmpty() ? QStringLiteral("Unable to inspect the Proton Drive file.") : output.standardError.trimmed();
        }

        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(output.standardOutput.toUtf8(), &parseError);
    // Storage sizes include encryption overhead and cannot verify local file contents.
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || !contentMetadata(document.object(), remotePath, file)) {
        if (error != nullptr) {
            *error = QStringLiteral("Proton Drive returned invalid file metadata.");
        }

        return false;
    }

    if (error != nullptr) {
        error->clear();
    }

    return true;
}

bool ProtonProvider::list(const QString &remotePath, QVector<RemoteItem> *items, QString *error)
{
    if (items == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("A destination for remote items is required.");
        }
        return false;
    }

    QJsonArray values;
    if (!listing(remotePath, &values, error)) return false;
    items->clear();
    for (const QJsonValue &value : values) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object = value.toObject();
        const QString name = itemName(object);
        const QString path = object.value(QStringLiteral("path")).toString(
            name.isEmpty() ? QString() : QDir(remotePath).filePath(name));
        const QString type = object.value(QStringLiteral("type")).toString().toLower();
        const bool directory = object.value(QStringLiteral("directory")).toBool(false)
            || object.value(QStringLiteral("is_dir")).toBool(false)
            || type == QStringLiteral("directory") || type == QStringLiteral("folder");
        if (name.isEmpty() && path.isEmpty()) {
            continue;
        }
        items->append({
            path,
            name.isEmpty() ? QFileInfo(path).fileName() : name,
            directory,
            static_cast<qint64>(object.value(QStringLiteral("size")).toDouble(
                object.value(QStringLiteral("totalStorageSize")).toDouble())),
            QDateTime::fromString(object.value(QStringLiteral("modified")).toString(
                object.value(QStringLiteral("modificationTime")).toString()), Qt::ISODateWithMs),
        });
    }
    return true;
}

bool ProtonProvider::listing(const QString &remotePath, QJsonArray *values, QString *error)
{
    const ProcessOutput output = runner.run({
        QStringLiteral("filesystem"), QStringLiteral("list"), QStringLiteral("-j"), remotePath,
    });
    if (!output.successful()) {
        if (error != nullptr) {
            *error = output.standardError.isEmpty() ? QStringLiteral("Unable to list the Proton Drive folder.") : output.standardError.trimmed();
        }
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(output.standardOutput.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("Proton Drive returned invalid folder metadata.");
        }
        return false;
    }

    if (document.isArray()) {
        *values = document.array();
    } else if (document.isObject() && (document.object().value("items").isArray()
            != document.object().value("entries").isArray())
        && !(document.object().contains("items") && document.object().contains("entries"))) {
        const QJsonObject object = document.object();
        *values = object.value(QStringLiteral("items")).toArray();
        if (!object.value("items").isArray()) {
            *values = object.value(QStringLiteral("entries")).toArray();
        }
    } else {
        if (error) *error = "Proton Drive returned an unsupported folder listing.";
        return false;
    }
    return true;
}

bool ProtonProvider::inspectDirectoryFiles(const QString &remotePath, QVector<RemoteFile> *files, QString *error)
{
    if (files == nullptr) return false;
    files->clear();
    QJsonArray values;
    if (!listing(remotePath, &values, error)) return false;
    const QString root = QDir::cleanPath(remotePath);
    QSet<QString> seen, ambiguous;
    for (const QJsonValue &value : values) {
        if (!value.isObject()) continue;
        const QJsonObject object = value.toObject();
        const QString name = itemName(object);
        if (name.isEmpty() || name.contains('/') || name == "." || name == "..") continue;
        const QString path = object.value(QStringLiteral("path")).toString(QDir(root).filePath(name));
        if (path != QDir::cleanPath(path) || QFileInfo(path).path() != root
            || QFileInfo(path).fileName() != name) continue;
        if (seen.contains(path)) ambiguous.insert(path);
        seen.insert(path);
        const QString type = object.value(QStringLiteral("type")).toString().toLower();
        if (type != QStringLiteral("file") || object.value(QStringLiteral("directory")).toBool()
            || object.value(QStringLiteral("is_dir")).toBool()) continue;
        RemoteFile file;
        if (contentMetadata(object, path, &file)) files->append(file);
    }
    files->removeIf([&](const RemoteFile &file) { return ambiguous.contains(file.path); });
    // Storage-only/empty listings cannot verify any files. Let callers disable
    // this optimization for the remaining directories in this verification.
    return !files->isEmpty();
}

bool ProtonProvider::trash(const QString &remotePath, QString *error)
{
    ensuredDirectories.clear();
    return run({QStringLiteral("filesystem"), QStringLiteral("trash"), remotePath}, error);
}

bool ProtonProvider::permanentlyDelete(const QString &remotePath, QString *error)
{
    ensuredDirectories.clear();
    return run({QStringLiteral("filesystem"), QStringLiteral("delete"), remotePath}, error);
}

bool ProtonProvider::run(const QStringList &arguments, QString *error) const
{
    const ProcessOutput output = runner.run(arguments);
    if (output.successful()) {
        return true;
    }

    if (error != nullptr) {
        *error = output.standardError.isEmpty() ? QStringLiteral("The Proton Drive command failed.") : output.standardError.trimmed();
    }

    return false;
}

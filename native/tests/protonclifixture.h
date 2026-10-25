#pragma once

#include "../src/processrunner.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QTemporaryDir>
#include <QUuid>
#include <sys/stat.h>

// Offline subset of cli-drive@0.8.0's filesystem commands. See
// protonclifixture.md for the checked CLI contract and deliberate limits.
class FilesystemRunner final : public ProcessRunner
{
public:
    enum class Failure { None, TransferError, PartialOutput, TruncatedOutput, CorruptOutput };

    QTemporaryDir remote;
    QStringList uploadedPaths;
    QVector<QStringList> calls;
    QStringList downloadedFolders;
    QStringList trashedPaths;
    QString failUploadName;
    QString truncateUploadName;
    Failure downloadFailure = Failure::None;
    bool includeSha256 = false;
    bool includeListingContentSize = false;
    QString omitListingMetadataName;
    QString failListPath;
    QString loseMutationResponse;
    bool rejectMutation = false;
    bool duplicateTrashName = false;
    QHash<QString, QString> globalAliases; // CLI UID segments bypass the apparent parent.
    QString wrongListedParentName;
    QString wrongResolvedParentPath;

    QString remoteFile(const QString &path) const
    {
        QString resolved = remote.path();
        for (const auto &part : path.split('/', Qt::SkipEmptyParts))
            resolved = globalAliases.contains(part) ? remote.filePath(globalAliases.value(part).mid(1)) : QDir(resolved).filePath(part);
        return resolved;
    }

    ProcessOutput run(const QStringList &arguments) override
    {
        calls.append(arguments);
        if (arguments.size() < 3 || arguments.first() != QStringLiteral("filesystem")) {
            return failure("Invalid CLI command");
        }
        const QString command = arguments.at(1);
        if (command == "upload" || command == "download") {
            QString fileStrategy;
            QString folderStrategy;
            QStringList paths;
            for (int index = 2; index < arguments.size(); ++index) {
                const QString argument = arguments.at(index);
                if (argument == "-j" || argument == "-t") continue;
                if (argument == "-f" || argument == "-d") {
                    if (++index >= arguments.size()) return failure("Missing conflict strategy");
                    (argument == "-f" ? fileStrategy : folderStrategy) = arguments.at(index);
                } else if (argument.startsWith('-')) {
                    return failure("Unsupported CLI option");
                } else {
                    paths.append(argument);
                }
            }
            const bool upload = command == "upload";
            const QStringList files = upload
                ? QStringList {"", "create-new-revision", "rename", "replace", "skip"}
                : QStringList {"rename", "remove", "skip"};
            const QStringList folders = upload
                ? QStringList {"", "merge", "rename", "replace", "skip"}
                : QStringList {"merge", "rename", "remove", "skip"};
            if (paths.size() < 2 || !files.contains(fileStrategy) || !folders.contains(folderStrategy)) {
                return failure("Unsupported transfer arguments");
            }
            const QString parent = upload ? remoteFile(paths.takeLast()) : paths.takeLast();
            if (!QFileInfo(parent).isDir()) return failure("Parent folder not found");
            for (const QString &path : paths) {
                const QString source = upload ? path : remoteFile(path);
                const QString name = QFileInfo(path).fileName();
                if (upload) uploadedPaths.append(path);
                else downloadedFolders.append(parent);
                Failure injected = upload ? Failure::None : downloadFailure;
                if (injected == Failure::TransferError) return failure("Connection interrupted");
                const auto output = transfer(source, QDir(parent).filePath(name), upload,
                    fileStrategy, folderStrategy, injected);
                if (!output.successful()) return output;
            }
            return {0, {}, {}};
        }
        const QString path = remoteFile(arguments.last());
        if (command == "info") {
            const QFileInfo file(path);
            if (!file.exists()) return failure("Node not found");
            QJsonObject info {{"uid", identity(path)}, {"parentUid", identity(file.absolutePath())},
                {"name", QJsonObject{{"ok", true}, {"value", file.fileName()}}},
                {"type", file.isDir() ? "folder" : "file"}, {"activeRevision", QJsonObject {{"claimedSize", file.size()}}}};
            if (arguments.last() == wrongResolvedParentPath) info["parentUid"] = "outside-parent";
            if (includeSha256 && file.isFile()) {
                QFile contents(path);
                if (!contents.open(QIODevice::ReadOnly)) return failure("Metadata read failed");
                info.insert("sha256", QString::fromLatin1(QCryptographicHash::hash(contents.readAll(), QCryptographicHash::Sha256).toHex()));
            }
            return {0, QString::fromUtf8(QJsonDocument(info).toJson()), {}};
        }
        if (command == "list") {
            if (arguments.last() == failListPath) return failure("Listing interrupted");
            if (arguments.last() == "/trash" && !QDir().mkpath(path)) return failure("Trash unavailable");
            if (!QFileInfo(path).isDir()) return failure("Folder not found");
            QJsonArray items;
            for (const QFileInfo &item : QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
                QJsonObject node {{"uid", identity(item.filePath())}, {"parentUid", identity(path)}, {"name", QJsonObject {{"ok", true}, {"value", item.fileName()}}},
                    {"type", item.isDir() ? "folder" : "file"}, {"totalStorageSize", item.size()},
                    {"modificationTime", item.lastModified().toUTC().toString(Qt::ISODateWithMs)}};
                if (item.fileName() == wrongListedParentName) node["parentUid"] = "outside-parent";
                if (item.isFile() && includeListingContentSize && item.fileName() != omitListingMetadataName) {
                    node.insert("activeRevision", QJsonObject {{"claimedSize", item.size()}});
                    if (includeSha256) {
                        QFile contents(item.filePath());
                        if (!contents.open(QIODevice::ReadOnly)) return failure("Metadata read failed");
                        node.insert("sha256", QString::fromLatin1(QCryptographicHash::hash(contents.readAll(), QCryptographicHash::Sha256).toHex()));
                    }
                }
                items.append(node);
                if (arguments.last() == "/trash" && duplicateTrashName) {
                    node["uid"] = "different-node";
                    items.append(node);
                }
            }
            return {0, QString::fromUtf8(QJsonDocument(items).toJson()), {}};
        }
        if ((command == "trash" || command == "delete") && arguments.size() == 4 && arguments[2] == "-j") {
            const QString uid = identity(path);
            if (uid.isEmpty()) return failure("Node not found");
            if (rejectMutation) return {0, QString::fromUtf8(QJsonDocument(QJsonArray{QJsonObject{{"uid", uid}, {"ok", false}}}).toJson()), {}};
            if (command == "trash") {
                if (arguments.last().startsWith("/trash/")) return failure("Already trashed");
                const QString trash = remoteFile("/trash");
                if (!QDir().mkpath(trash) || !QDir().rename(path, QDir(trash).filePath(QFileInfo(path).fileName()))) return failure("Trash conflict");
                trashedPaths.append(arguments.last());
            } else {
                if (!arguments.last().startsWith("/trash/") || !remove(path)) return failure("Only trashed nodes can be deleted");
            }
            if (loseMutationResponse == command) return failure("Lost mutation response");
            return {0, QString::fromUtf8(QJsonDocument(QJsonArray{QJsonObject{{"uid", uid}, {"ok", true}}}).toJson()), {}};
        }
        if (command == "create-folder" && arguments.size() == 4) {
            const QString parent = remoteFile(arguments.at(2));
            return QFileInfo(parent).isDir() && QDir(parent).mkdir(arguments.last())
                ? ProcessOutput {0, {}, {}} : failure("Folder creation failed");
        }
        return failure("Unsupported CLI command");
    }

private:
    static QString identity(const QString &path)
    {
        struct stat info{};
        if (::lstat(QFile::encodeName(path).constData(), &info) != 0) return {};
        return QString::number(info.st_dev) + ':' + QString::number(info.st_ino);
    }
    static ProcessOutput failure(const QString &message) { return {1, {}, message}; }

    static bool remove(const QString &path)
    {
        return QFileInfo(path).isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
    }

    bool discard(const QString &path, bool upload)
    {
        if (!upload) return remove(path);
        // Upload replace trashes the old node; download remove deletes it.
        const QString trash = remote.filePath(".fixture-trash");
        if (!QDir().mkpath(trash)) return false;
        const QString target = QDir(trash).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (!QDir().rename(path, target)) return false;
        trashedPaths.append(target);
        return true;
    }

    ProcessOutput transfer(const QString &source, QString destination, bool upload,
        const QString &fileStrategy, const QString &folderStrategy, Failure injected)
    {
        const QFileInfo input(source);
        if (!input.exists() || input.isSymLink()) return failure("Source not found or unsupported");
        // Apply named upload failures to descendants too, so a recursive upload
        // can genuinely stop after transferring only part of its prepared tree.
        if (upload && input.fileName() == failUploadName) return failure("Connection interrupted");
        if (upload && input.fileName() == truncateUploadName) injected = Failure::TruncatedOutput;
        if (QFileInfo::exists(destination)) {
            if (upload && input.isFile() && QFileInfo(destination).isFile()) {
                QFile original(source), existing(destination);
                if (original.open(QIODevice::ReadOnly) && existing.open(QIODevice::ReadOnly)
                    && original.readAll() == existing.readAll()) return {0, {}, {}};
            }
            const QString strategy = input.isDir() ? folderStrategy : fileStrategy;
            if (strategy.isEmpty()) return failure("Unresolved conflict in noninteractive JSON mode");
            if (strategy == "skip") return {0, {}, {}};
            if (strategy == "rename") {
                const QString base = destination;
                int suffix = 1;
                do { destination = base + "." + QString::number(suffix++); }
                while (QFileInfo::exists(destination));
            } else if (strategy == "merge") {
                if (!QFileInfo(destination).isDir()) return failure("Cannot merge into a file");
            } else if (strategy == "create-new-revision") {
                if (!QFileInfo(destination).isFile() || !QFile::remove(destination)) {
                    return failure("Cannot create a file revision");
                }
            } else if (!discard(destination, upload)) {
                return failure("Conflict removal failed");
            }
        }
        if (input.isDir()) {
            if (!QDir().mkpath(destination)) return failure("Folder transfer failed");
            for (const QFileInfo &child : QDir(source).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
                const auto output = transfer(child.filePath(), QDir(destination).filePath(child.fileName()),
                    upload, fileStrategy, folderStrategy, injected);
                if (!output.successful()) return output;
            }
            return {0, {}, {}};
        }
        if (!QFile::copy(source, destination)) return failure("Transfer failed");
        if (injected != Failure::None) {
            QFile file(destination);
            // Local staging is read-only, but remote failure injection must be
            // able to alter the copied bytes regardless of POSIX source modes.
            if (!file.setPermissions(file.permissions() | QFileDevice::WriteOwner)) return failure("Failure injection failed");
            if (!file.open(QIODevice::ReadWrite)) return failure("Failure injection failed");
            if (injected == Failure::CorruptOutput) {
                if (file.size() == 0 || file.write("!") != 1) return failure("Failure injection failed");
            } else if (!file.resize(1)) {
                return failure("Failure injection failed");
            }
            if (injected == Failure::PartialOutput) return failure("Connection interrupted after partial output");
        }
        return {0, {}, {}};
    }
};

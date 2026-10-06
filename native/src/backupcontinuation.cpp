#include "backupcontinuation.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
bool syncDirectory(const QString &path)
{
#ifdef Q_OS_UNIX
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY);
    const bool synced = fd >= 0 && ::fsync(fd) == 0;
    if (fd >= 0) ::close(fd);
    return synced;
#else
    Q_UNUSED(path);
    return true;
#endif
}

bool publish(const QString &path, const QJsonObject &object, QString *error)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = QStringLiteral("Unable to commit backup checkpoint: %1").arg(file.errorString());
        return false;
    }
    // Persist the rename too, so reboot cannot lose an acknowledged checkpoint.
    if (!syncDirectory(QFileInfo(path).absolutePath())) {
        if (error) *error = QStringLiteral("Unable to durably publish the backup checkpoint.");
        return false;
    }
    return true;
}

bool readObject(const QString &path, QJsonObject *object, QString *error)
{
    QFile file(path);
    QJsonParseError parse;
    if (file.open(QIODevice::ReadOnly)) {
        const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
        if (file.error() == QFileDevice::NoError && parse.error == QJsonParseError::NoError && document.isObject()) {
            *object = document.object();
            return true;
        }
    }
    if (error) *error = QStringLiteral("The backup checkpoint is unreadable or malformed: %1").arg(path);
    return false;
}
}

BackupContinuation::BackupContinuation(QString directory) : directory(std::move(directory)) {}

bool BackupContinuation::open(const QString &root, const BackupCopyMetadata &identity, QString *error, int format)
{
    remoteRoot = root;
    metadata = identity;
    payloadFormat = format;
    if (directory.isEmpty()) return true;
    QString existingAncestor = QFileInfo(directory).absoluteFilePath();
    while (!QFileInfo::exists(existingAncestor) && existingAncestor != "/")
        existingAncestor = QFileInfo(existingAncestor).absolutePath();
    if (!QDir().mkpath(directory) || QFileInfo(directory).isSymLink()
        || !QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        if (error) *error = QStringLiteral("Unable to create the backup checkpoint directory.");
        return false;
    }
    // New namespace directories need their parent entries synced too; syncing
    // only the batch's rename would not make a newly created journal durable.
    QString created = QFileInfo(directory).absoluteFilePath();
    while (created != existingAncestor) {
        if (!syncDirectory(created) || !syncDirectory(QFileInfo(created).absolutePath())) {
            if (error) *error = QStringLiteral("Unable to durably create the backup checkpoint namespace.");
            return false;
        }
        created = QFileInfo(created).absolutePath();
    }
    const QString headerPath = QDir(directory).filePath(QStringLiteral("identity.json"));
    if (!QFileInfo::exists(headerPath)) return saveRoots(error);
    QJsonObject header;
    if (!readObject(headerPath, &header, error)) return false;
    if (header.value("version").toInt() != 1 || header.value("payload_format").toInt(2) != payloadFormat
        || header.value("remote_root").toString() != root
        || header.value("set_id").toString() != identity.setId || header.value("copy_id").toString() != identity.copyId) {
        if (error) *error = QStringLiteral("The backup checkpoint belongs to another copy or has an unsupported version.");
        return false;
    }
    metadata = {header.value("computer").toString(), identity.setId, header.value("set_name").toString(),
        identity.copyId, QDateTime::fromString(header.value("created_at").toString(), Qt::ISODateWithMs)};
    if (!metadata.createdAt.isValid()) {
        if (error) *error = QStringLiteral("The backup checkpoint has an invalid creation time.");
        return false;
    }
    const auto rootMappings = header.value("roots").toObject();
    for (auto it = rootMappings.begin(); it != rootMappings.end(); ++it) {
        const QString prefix = it.value().toString();
        if (!it.key().startsWith('/') || !it.value().isString() || prefix.contains('/') || prefix == "." || prefix == "..") {
            if (error) *error = QStringLiteral("The backup checkpoint contains invalid root mappings.");
            return false;
        }
        roots.insert(it.key(), prefix);
    }
    const auto batches = QDir(directory).entryList({QStringLiteral("batch-*.json")}, QDir::Files, QDir::Name);
    for (const QString &batch : batches) {
        QJsonObject object;
        if (!readObject(QDir(directory).filePath(batch), &object, error)) return false;
        if (object.value("version").toInt() != 1 || !object.value("entries").isArray()) {
            if (error) *error = QStringLiteral("The backup checkpoint contains an invalid batch.");
            return false;
        }
        for (const auto &value : object.value("entries").toArray()) {
            const auto item = value.toObject();
            BackupEntry entry {item.value("source").toString(), item.value("remote").toString(),
                item.value("size").toInteger(-1), QByteArray::fromHex(item.value("sha256").toString().toLatin1()),
                item.value("restore").toString()};
            if (!entry.sourcePath.startsWith('/') || entry.size < 0 || entry.checksum.size() != 32
                || entry.restorePath.isEmpty() || entry.restorePath.startsWith('/')
                || entry.restorePath.split('/').contains("..")
                || entry.restorePath != QDir::cleanPath(entry.restorePath)
                || !entry.remotePath.startsWith(root + '/') || entry.remotePath == QDir(root).filePath("manifest.json")
                || entry.remotePath != QDir::cleanPath(entry.remotePath) || entry.remotePath.split('/').contains("..")) {
                if (error) *error = QStringLiteral("The backup checkpoint contains an invalid payload identity.");
                return false;
            }
            mappings.insert(entry.sourcePath, entry);
            if (object.value("verified").toBool(true)) verified.insert(entry.sourcePath, entry);
        }
        bool valid = false;
        const int index = batch.mid(6, batch.size() - 11).toInt(&valid);
        if (!valid || index != nextBatch++) {
            if (error) *error = QStringLiteral("The backup checkpoint batch sequence is incomplete.");
            return false;
        }
    }
    return true;
}

bool BackupContinuation::saveRoots(QString *error)
{
    if (directory.isEmpty()) return true;
    QJsonObject mappings;
    for (auto it = roots.cbegin(); it != roots.cend(); ++it) mappings.insert(it.key(), it.value());
    return publish(QDir(directory).filePath("identity.json"), {
        {"version", 1}, {"payload_format", payloadFormat}, {"remote_root", remoteRoot}, {"set_id", metadata.setId}, {"copy_id", metadata.copyId},
        {"computer", metadata.computerName}, {"set_name", metadata.setName},
        {"created_at", metadata.createdAt.toString(Qt::ISODateWithMs)}, {"roots", mappings}}, error);
}

bool BackupContinuation::checkpoint(const QVector<BackupEntry> &entries, QString *error, bool verifiedPayloads)
{
    if (entries.isEmpty()) return true;
    QJsonArray items;
    for (const BackupEntry &entry : entries) {
        items.append(QJsonObject {{"source", entry.sourcePath}, {"remote", entry.remotePath}, {"size", entry.size},
            {"sha256", QString::fromLatin1(entry.checksum.toHex())}, {"restore", entry.restorePath}});
    }
    if (!directory.isEmpty() && !publish(QDir(directory).filePath(
            QStringLiteral("batch-%1.json").arg(nextBatch, 8, 10, QChar('0'))),
            {{"version", 1}, {"entries", items}, {"verified", verifiedPayloads}}, error)) return false;
    ++nextBatch;
    for (const BackupEntry &entry : entries) {
        mappings.insert(entry.sourcePath, entry);
        if (verifiedPayloads) verified.insert(entry.sourcePath, entry);
    }
    return true;
}

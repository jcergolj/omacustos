#include "settingsarchive.h"
#include <QDir>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>

namespace {
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
bool component(const QString &name)
{
    if (name.isEmpty() || name == "." || name == "..") return false;
    for (auto ch : name) if (!ch.isLetterOrNumber() && ch != '-' && ch != '_' && ch != '.') return false;
    return true;
}
bool children(const QString &parent, const QVector<RemoteItem> &items, QString *error)
{
    QSet<QString> seen;
    if (items.size() > 10000) return fail(error, "Too many settings versions to browse safely.");
    for (const auto &item : items) {
        if (!component(item.name) || item.path != parent + '/' + item.name || seen.contains(item.path))
            return fail(error, "Unsafe or ambiguous settings listing. No settings were imported.");
        seen.insert(item.path);
    }
    return true;
}
bool selection(const QString &path)
{
    if (!path.startsWith(SettingsArchive::root() + '/')) return false;
    const auto parts = path.mid(SettingsArchive::root().size() + 1).split('/');
    return parts.size() == 2 && component(parts[0]) && component(parts[1]) && parts[1].endsWith(".json");
}
}

bool SettingsArchive::exportSettings(BackupProvider &provider, const BackupConfig &config,
    const QString &computer, QString *remotePath, QString *error)
{
    if (remotePath) remotePath->clear();
    QString label;
    for (auto ch : computer) label += ch.isLetterOrNumber() || ch == '-' || ch == '_' ? ch : QChar('_');
    if (label.isEmpty()) label = "computer";
    QTemporaryDir temporary;
    if (!temporary.isValid()) return fail(error, "Unable to create a private settings workspace.");
    const QString folder = root() + '/' + label;
    const QString version = QDateTime::currentDateTimeUtc().toString("yyyyMMdd'T'HHmmsszzz'Z'")
        + '-' + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json";
    const QString target = folder + '/' + version;
    const QString source = temporary.filePath(version);
    if (!BackupConfigStore(source).exportSets(config, error)) return false;
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, "Unable to read the settings export.");
    const auto contents = file.read(BackupConfigStore::MaximumImportBytes + 1);
    if (contents.size() > BackupConfigStore::MaximumImportBytes) return fail(error, "Settings exports must not exceed 4 MiB.");
    if (!provider.ensureDirectory(folder, error) || !provider.uploadImmutable(source, target, error)) return false;
    const QString verified = temporary.filePath("verified.json");
    if (!download(provider, target, verified, error)) return false;
    QFile received(verified);
    if (!received.open(QIODevice::ReadOnly) || received.read(BackupConfigStore::MaximumImportBytes + 1) != contents)
        return fail(error, "Settings verification mismatch. Earlier exports are untouched; this version must not be used.");
    if (remotePath) *remotePath = target;
    return true;
}

bool SettingsArchive::browse(BackupProvider &provider, QVariantList *versions, QString *error)
{
    if (!versions) return fail(error, "A destination for settings versions is required.");
    QVector<RemoteItem> computers;
    QVariantList result;
    if (!provider.listCompleteIfPresent(root(), &computers, error) || !children(root(), computers, error)) return false;
    for (const auto &computer : computers) {
        if (!computer.directory) return fail(error, "Unexpected file in the settings computer listing.");
        QVector<RemoteItem> files;
        if (!provider.listComplete(computer.path, &files, error) || !children(computer.path, files, error)) return false;
        for (const auto &file : files) {
            if (file.directory || !file.name.endsWith(".json") || file.size < 0 || file.size > BackupConfigStore::MaximumImportBytes)
                return fail(error, "Invalid or oversized settings version in Proton Drive.");
            result.append(QVariantMap{{"computer", computer.name}, {"version", file.name}, {"path", file.path}});
            if (result.size() > 10000) return fail(error, "Too many settings versions to browse safely.");
        }
    }
    std::sort(result.begin(), result.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("path").toString() > b.toMap().value("path").toString();
    });
    *versions = result;
    return true;
}

bool SettingsArchive::download(BackupProvider &provider, const QString &remotePath,
    const QString &localPath, QString *error)
{
    if (!selection(remotePath)) return fail(error, "Unsafe settings version selection.");
    // Re-enumerate instead of trusting a cached UI row, including duplicate names.
    QVariantList versions;
    if (!browse(provider, &versions, error)) return false;
    if (std::none_of(versions.cbegin(), versions.cend(), [&](const QVariant &v) { return v.toMap().value("path") == remotePath; }))
        return fail(error, "The selected settings version no longer exists.");
    RemoteFile metadata;
    if (!provider.inspect(remotePath, &metadata, error)) return false;
    if (metadata.path != remotePath) return fail(error, "Ambiguous settings file metadata.");
    if (metadata.size < 0 || metadata.size > BackupConfigStore::MaximumImportBytes)
        return fail(error, "Settings imports must not exceed 4 MiB.");
    if (!provider.download(remotePath, localPath, error)) return false;
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly) || file.read(BackupConfigStore::MaximumImportBytes + 1).size() > BackupConfigStore::MaximumImportBytes)
        return fail(error, "Unreadable or oversized settings download.");
    BackupConfig validated;
    return BackupConfigStore(localPath).importSets(&validated, error);
}

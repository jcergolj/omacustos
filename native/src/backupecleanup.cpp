#include "backupecleanup.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace {

QStringList stringList(const QJsonValue &value)
{
    QStringList result;
    for (const QJsonValue &item : value.toArray()) {
        if (item.isString()) {
            result.append(item.toString());
        }
    }
    return result;
}

QJsonArray jsonStrings(const QStringList &values)
{
    QJsonArray result;
    for (const QString &value : values) {
        result.append(value);
    }
    return result;
}

bool inside(const QString &path, const QString &root)
{
    const QString cleanPath = QDir::cleanPath(path);
    const QString cleanRoot = QDir::cleanPath(root);
    return cleanPath == cleanRoot || (cleanRoot == QStringLiteral("/")
        ? cleanPath.startsWith('/')
        : cleanPath.startsWith(cleanRoot + QDir::separator()));
}

bool alreadyGone(const QString &error)
{
    const QString message = error.toLower();
    return message.contains(QStringLiteral("not found"))
        || message.contains(QStringLiteral("does not exist"))
        || message.contains(QStringLiteral("unavailable"));
}

}

CleanupStore::CleanupStore(QString path)
    : path(std::move(path))
{
}

bool CleanupStore::load(QString *error, QByteArray *contents)
{
    QFile file(path);
    if (!file.exists()) {
        cleanupStates.clear();
        if (contents) *contents = QByteArray(1, '\0');
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("The cleanup state could not be opened.");
        }
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("The cleanup state could not be read.");
        }
        return false;
    }
    if (!loadFromBytes(bytes, error)) {
        return false;
    }
    if (contents) *contents = QByteArray(1, '\1') + bytes;
    return true;
}

bool CleanupStore::loadFromBytes(const QByteArray &contents, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(contents, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("The cleanup state is malformed.");
        }
        return false;
    }
    const QJsonValue setsValue = document.object().value(QStringLiteral("sets"));
    if (!setsValue.isUndefined() && !setsValue.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("The cleanup state is malformed.");
        }
        return false;
    }
    QHash<QString, CleanupState> nextStates;
    const QJsonObject sets = setsValue.toObject();
    for (auto iterator = sets.constBegin(); iterator != sets.constEnd(); ++iterator) {
        if (iterator.key().isEmpty() || !iterator.value().isObject()) {
            if (error != nullptr) {
                *error = QStringLiteral("The cleanup state contains an invalid record.");
            }
            return false;
        }
        const QJsonObject object = iterator.value().toObject();
        nextStates.insert(iterator.key(), {
            object.value(QStringLiteral("decision")).toString(QStringLiteral("pending")),
            stringList(object.value(QStringLiteral("targets"))),
            stringList(object.value(QStringLiteral("trashed"))),
            stringList(object.value(QStringLiteral("completed"))),
            object.value(QStringLiteral("last_error")).toString(),
        });
    }
    cleanupStates = std::move(nextStates);
    return true;
}

bool CleanupStore::save(QString *error) const
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to create the cleanup state directory.");
        }
        return false;
    }
    QJsonObject sets;
    for (auto iterator = cleanupStates.cbegin(); iterator != cleanupStates.cend(); ++iterator) {
        sets.insert(iterator.key(), QJsonObject {
            {QStringLiteral("decision"), iterator.value().decision},
            {QStringLiteral("targets"), jsonStrings(iterator.value().targets)},
            {QStringLiteral("trashed"), jsonStrings(iterator.value().trashed)},
            {QStringLiteral("completed"), jsonStrings(iterator.value().completed)},
            {QStringLiteral("last_error"), iterator.value().lastError},
        });
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to write the cleanup state.");
        }
        return false;
    }
    const QByteArray contents = QJsonDocument(QJsonObject {{QStringLiteral("sets"), sets}}).toJson(QJsonDocument::Indented);
    if (file.write(contents) != contents.size() || !file.commit()) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to finish writing the cleanup state.");
        }
        return false;
    }
    return true;
}

CleanupState CleanupStore::state(const QString &setId) const
{
    return cleanupStates.value(setId);
}

void CleanupStore::setPending(const QString &setId, const QStringList &targets)
{
    CleanupState &state = cleanupStates[setId];
    if (!state.targets.isEmpty() || state.decision == QStringLiteral("confirmed")) {
        return;
    }
    state.decision = QStringLiteral("pending");
    state.targets = targets;
    state.trashed.clear();
    state.completed.clear();
    state.lastError.clear();
}

void CleanupStore::confirm(const QString &setId)
{
    CleanupState &state = cleanupStates[setId];
    state.decision = QStringLiteral("confirmed");
}

void CleanupStore::setTargets(const QString &setId, const QStringList &targets)
{
    CleanupState &state = cleanupStates[setId];
    if (state.decision != QStringLiteral("confirmed") || !state.targets.isEmpty()) {
        return;
    }
    state.targets = targets;
    state.trashed.clear();
    state.completed.clear();
    state.lastError.clear();
}

QString CleanupStore::filePath() const
{
    return path;
}

QHash<QString, CleanupState> &CleanupStore::states()
{
    return cleanupStates;
}

QStringList BackupCleanup::eligibleTargets(const QVector<RemoteCopy> &copies, int retention,
    const QString &computerName, const QString &setId)
{
    QVector<RemoteCopy> successful;
    QStringList targets;
    for (const RemoteCopy &copy : copies) {
        if ((!computerName.isEmpty() && copy.computerName != computerName)
            || (!setId.isEmpty() && copy.setId != setId)) {
            continue;
        }
        if (copy.status == QStringLiteral("complete") && copy.complete()) {
            successful.append(copy);
        }
    }
    std::sort(successful.begin(), successful.end(), [](const RemoteCopy &left, const RemoteCopy &right) {
        return left.createdAt > right.createdAt;
    });
    const int keep = qMax(0, retention);
    for (int index = keep; index < successful.size(); ++index) {
        targets.append(successful.at(index).rootPath);
    }
    for (const RemoteCopy &copy : copies) {
        if ((!computerName.isEmpty() && copy.computerName != computerName)
            || (!setId.isEmpty() && copy.setId != setId)) {
            continue;
        }
        if (copy.status == QStringLiteral("incomplete")) {
            targets.append(copy.rootPath);
        }
    }
    targets.removeDuplicates();
    targets.sort();
    return targets;
}

bool BackupCleanup::apply(BackupProvider &provider, CleanupStore &store, const QString &setId,
    const QString &allowedRoot, QString *error)
{
    CleanupState &state = store.states()[setId];
    if (state.decision != QStringLiteral("confirmed")) {
        return true;
    }

    for (const QString &target : state.targets) {
        if (state.completed.contains(target)) {
            continue;
        }
        if (!allowedRoot.isEmpty() && !inside(target, allowedRoot)) {
            state.lastError = QStringLiteral("The cleanup target is outside the configured backup.");
            if (error != nullptr) {
                *error = state.lastError;
            }
            store.save();
            return false;
        }
        QString providerError;
        if (!state.trashed.contains(target) && !provider.trash(target, &providerError)) {
            state.lastError = providerError;
            if (error != nullptr) {
                *error = providerError;
            }
            store.save();
            return false;
        }
        if (!state.trashed.contains(target)) {
            state.trashed.append(target);
            if (!store.save(error)) {
                return false;
            }
        }
        if (!provider.permanentlyDelete(target, &providerError) && !alreadyGone(providerError)) {
            state.lastError = providerError;
            if (error != nullptr) {
                *error = providerError;
            }
            store.save();
            return false;
        }
        state.completed.append(target);
        state.lastError.clear();
        if (!store.save(error)) {
            return false;
        }
    }
    state.targets.clear();
    state.trashed.clear();
    state.completed.clear();
    return store.save(error);
}

#include "backupecleanup.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>

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
    return cleanPath != cleanRoot && (cleanRoot == QStringLiteral("/")
        ? cleanPath.startsWith('/')
        : cleanPath.startsWith(cleanRoot + QDir::separator()));
}

bool alreadyGone(const QString &error)
{
    const QString message = error.toLower();
    return message.contains(QStringLiteral("not found"))
        || message.contains(QStringLiteral("does not exist"));
}

}

CleanupStore::CleanupStore(QString path)
    : path(QFileInfo(path).absoluteFilePath())
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
            object.value(QStringLiteral("proposal_id")).toString(),
        });
    }
    cleanupStates = std::move(nextStates);
    return true;
}

bool CleanupStore::lockAndLoad(QLockFile &lock, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to create the cleanup state directory. Check directory permissions and retry.");
        }
        return false;
    }
    // Remote calls may take longer than QLockFile's default stale timeout.
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to lock cleanup state. Wait for active cleanup to finish, check directory permissions, and retry.");
        }
        return false;
    }
    return load(error);
}

bool CleanupStore::save(QString *error) const
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to create the cleanup state directory.");
        }
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to write the cleanup state. Check directory permissions and free space, then retry.");
        }
        return false;
    }
    const QByteArray contents = toBytes();
    if (file.write(contents) != contents.size() || !file.commit()) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to finish writing the cleanup state. Check directory permissions and free space, then retry.");
        }
        return false;
    }
    return true;
}

QByteArray CleanupStore::toBytes() const
{
    QJsonObject sets;
    for (auto iterator = cleanupStates.cbegin(); iterator != cleanupStates.cend(); ++iterator) {
        sets.insert(iterator.key(), QJsonObject {
            {QStringLiteral("decision"), iterator.value().decision},
            {QStringLiteral("targets"), jsonStrings(iterator.value().targets)},
            {QStringLiteral("trashed"), jsonStrings(iterator.value().trashed)},
            {QStringLiteral("completed"), jsonStrings(iterator.value().completed)},
            {QStringLiteral("last_error"), iterator.value().lastError},
            {QStringLiteral("proposal_id"), iterator.value().proposalId},
        });
    }
    return QJsonDocument(QJsonObject {{QStringLiteral("sets"), sets}}).toJson(QJsonDocument::Indented);
}

CleanupState CleanupStore::state(const QString &setId) const
{
    return cleanupStates.value(setId);
}

bool CleanupStore::propose(const QString &setId, const QStringList &targets, QString *error)
{
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lockAndLoad(lock, error)) {
        return false;
    }
    CleanupState &state = cleanupStates[setId];
    // Once authorized, a retry must never replace or expand an unfinished scope.
    if (state.decision == QStringLiteral("confirmed") && !state.targets.isEmpty()) {
        return true;
    }
    if (state.targets == targets) {
        return true;
    }
    state.targets = targets;
    state.trashed.clear();
    state.completed.clear();
    state.lastError.clear();
    state.proposalId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!save(error)) {
        load();
        return false;
    }
    return true;
}

bool CleanupStore::confirm(const QString &setId, const CleanupState &presented, QString *error)
{
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lockAndLoad(lock, error)) {
        return false;
    }
    CleanupState &state = cleanupStates[setId];
    if (state.targets.isEmpty() || state.decision != QStringLiteral("pending")
        || state.targets != presented.targets || state.decision != presented.decision
        || state.proposalId != presented.proposalId) {
        if (error != nullptr) {
            *error = QStringLiteral("The cleanup proposal changed. Review the refreshed targets before confirming.");
        }
        return false;
    }
    state.decision = QStringLiteral("confirmed");
    if (!save(error)) {
        load();
        return false;
    }
    return true;
}

bool CleanupStore::forgetTarget(const QString &setId, const QString &target, QString *error)
{
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lockAndLoad(lock, error)) {
        return false;
    }
    auto iterator = cleanupStates.find(setId);
    if (iterator == cleanupStates.end() || !iterator->targets.contains(target)) {
        return true;
    }
    iterator->targets.removeAll(target);
    iterator->trashed.removeAll(target);
    iterator->completed.removeAll(target);
    iterator->proposalId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!save(error)) {
        load();
        return false;
    }
    return true;
}

QString CleanupStore::filePath() const
{
    return path;
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

bool BackupCleanup::run(BackupProvider &provider, CleanupStore &store, const QString &setId,
    const QStringList &targets, const QString &allowedRoot, QString *error)
{
    return store.propose(setId, targets, error) && apply(provider, store, setId, allowedRoot, error);
}

bool BackupCleanup::apply(BackupProvider &provider, CleanupStore &store, const QString &setId,
    const QString &allowedRoot, QString *error)
{
    // Keep the lock through remote operations and phase writes. All writers load
    // fresh state while locked, so UI snapshots cannot overwrite worker progress.
    QLockFile lock(store.path + QStringLiteral(".lock"));
    if (!store.lockAndLoad(lock, error)) {
        return false;
    }
    CleanupState &state = store.cleanupStates[setId];
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
        if (!state.trashed.contains(target) && !provider.trash(target, &providerError)
            && !alreadyGone(providerError)) {
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

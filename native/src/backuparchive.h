#pragma once

#include "backupmanifest.h"
#include <functional>

// Archive implementation never writes into a restore destination. Paths returned
// by extract are operation-owned, private regular files, ready for atomic placement.
namespace BackupArchiveIO {
bool safeMember(const QString &path);
bool pack(const QString &tree, const QString &remoteRoot, const QString &output, qint64 maxBytes,
    QVector<BackupEntry> *entries, QString *error, const std::function<bool()> &stopped);
bool extract(const QString &input, const QVector<BackupEntry> &selected,
    const QString &workspace, QVector<BackupEntry> *payloads, QString *error);
bool verify(const QString &path, qint64 size, const QByteArray &checksum);
}

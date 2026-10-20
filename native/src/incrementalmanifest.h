#pragma once

#include <QByteArray>
#include <QString>

// Pure v4 contract validation only. This does not authorize remote mutations,
// certify receipt authenticity. IncrementalRestore is the separate v4 adapter.
class IncrementalManifest
{
public:
    static bool validId(const QString &value);
    static bool safeRelativePath(const QString &value);
    static bool validate(const QByteArray &json, const QString &setId,
        const QString &snapshotId, QString *error = nullptr);
};

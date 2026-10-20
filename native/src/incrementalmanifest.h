#pragma once

#include <QByteArray>
#include <QString>

// Pure v4 contract validation only. This does not authorize remote mutations,
// certify receipt authenticity, or enable v4 in the legacy restore pipeline.
class IncrementalManifest
{
public:
    static bool validate(const QByteArray &json, const QString &setId,
        const QString &snapshotId, QString *error = nullptr);
};

#pragma once

#include <QStringList>
#include <QLockFile>
#include <memory>

// Private, owned payload workspace. Recovery only removes marked direct
// children after acquiring their lifetime lock; arbitrary configured directories
// and source paths are never cleanup targets.
class BackupStaging final
{
public:
    ~BackupStaging();
    static bool validate(const QString &directory, const QStringList &sources, QString *error);
    bool open(const QString &configuredDirectory, const QStringList &sources, QString *error);
    bool reset(QString *error);
    bool hasSpace(qint64 bytes, QString *error) const;
    QString path() const { return workspace; }
    QString root() const { return base; }

private:
    QString base;
    QString workspace;
    std::unique_ptr<QLockFile> lock;
};

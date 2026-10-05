#pragma once

#include <QString>
#include <QVector>

struct BackupIssue {
    QString path;
    QString phase;
    QString reason;
};

struct BackupResult {
    bool reported = false;
    bool manifestVerified = false;
    int verifiedFiles = 0;
    qint64 verifiedBytes = 0;
    QVector<BackupIssue> issues;
    // Recoverable interruption is distinct from a finalized incomplete copy.
    bool interrupted = false;
    bool waitingForSpace = false;
};

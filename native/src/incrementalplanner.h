#pragma once

#include "backupresult.h"

#include <QDateTime>
#include <QJsonArray>
#include <QStringList>
#include <functional>

// A completed scanner supplies the selected regular files and stable restore
// mappings. Missing/unreadable scan roots must fail the scan, not become removals.
struct IncrementalSource {
    QString sourcePath;
    QString restorePath;
};

struct IncrementalPlannedFile {
    enum class Change { Added, Changed, Reused };
    Change change = Change::Added;
    QString sourcePath;
    QString restorePath;
    qint64 size = 0;
    QByteArray checksum;
    int mode = 0;
    QString mtimeNs;
    QString archiveId;
    QString memberPath;
    bool requiresUpload() const { return archiveId.isEmpty(); }
};

struct IncrementalPlanRequest {
    QString setId;
    QVector<IncrementalSource> sources;
    QVector<BackupIssue> scanIssues; // An incomplete scan never authorizes removals.
    QByteArray previousManifest; // Empty means the first baseline, not missing history.
    QString previousSnapshotId;
    QDateTime checkedAt;
    QDateTime lastSuccessfulCheck;
    // Cooperative cancellation; checked before opening and during bounded reads.
    std::function<bool()> stopped;
};

struct IncrementalPlan {
    bool success = false;
    bool snapshotRequired = false;
    QVector<IncrementalPlannedFile> files; // Restore-path order; disjoint Change categories.
    QStringList removed; // Restore-path order.
    QJsonArray requiredArchives; // Object-ID order, complete inventories/receipts.
    QDateTime lastSuccessfulCheck;
    QDateTime lastSavedChange; // Never advanced by a plan: only publication saves a change.
    QVector<BackupIssue> issues;
};

// Read-only local source capture and pure comparison with exactly one snapshot.
// No provider, temporary files, queue writes, retention, or publication authority.
class IncrementalPlanner
{
public:
    static IncrementalPlan plan(const IncrementalPlanRequest &request);
    // Recapture into an already private workspace. Exact planned bytes and restore
    // metadata must match; a failed/partial output is removed, never uploaded.
    static bool stage(const IncrementalPlannedFile &expected, const QString &outputPath,
        QString *error, const std::function<bool()> &stopped = {});
};

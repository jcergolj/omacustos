#include "incrementalplanner.h"
#include "incrementalmanifest.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>

namespace {
const QString setId = "10000000-0000-4000-8000-000000000001";
const QString snapshotId = "30000000-0000-4000-8000-000000000001";
const QString objectId = "40000000-0000-4000-8000-000000000001";
const QString secondObjectId = "40000000-0000-4000-8000-000000000002";
const auto savedAt = QDateTime::fromString("2026-10-19T10:00:00.000Z", Qt::ISODateWithMs);
const auto checkedAt = savedAt.addDays(1);

bool write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}

bool setTime(const QString &path, qint64 seconds = 1700000000, long nanoseconds = 123456789)
{
    const timespec times[2] = {{seconds, nanoseconds}, {seconds, nanoseconds}};
    return ::utimensat(AT_FDCWD, QFile::encodeName(path).constData(), times, 0) == 0;
}

IncrementalPlanRequest request(const QString &directory, const QStringList &names)
{
    IncrementalPlanRequest result;
    result.setId = setId;
    result.checkedAt = checkedAt;
    for (const auto &name : names) result.sources.append({directory + '/' + name, name});
    return result;
}

QJsonObject manifest(const IncrementalPlan &plan)
{
    QJsonArray entries, members;
    int index = 0;
    for (const auto &file : plan.files) {
        const QString member = "member-" + QString::number(index++);
        members.append(QJsonObject{{"path", member}, {"size", file.size}, {"sha256", QString::fromLatin1(file.checksum.toHex())}});
        entries.append(QJsonObject{{"restore", file.restorePath}, {"archive", objectId}, {"member", member},
            {"size", file.size}, {"sha256", QString::fromLatin1(file.checksum.toHex())},
            {"mode", file.mode}, {"mtime_ns", file.mtimeNs}});
    }
    const QString digest(64, 'a');
    QJsonArray archives;
    if (!members.isEmpty()) archives.append(QJsonObject{{"id", objectId}, {"remote", "objects/" + objectId + ".tar.gz"},
        {"size", 128}, {"sha256", digest}, {"members", members},
        {"original_verification", QJsonObject{{"method", "download-sha256"}, {"verified_at", savedAt.toString(Qt::ISODateWithMs)}, {"sha256", digest}}}});
    return {{"version", 4}, {"application", "omacustos"}, {"set_id", setId},
        {"origin_computer_id", "20000000-0000-4000-8000-000000000001"},
        {"owner_computer_id", "20000000-0000-4000-8000-000000000001"}, {"ownership_epoch", 1},
        {"snapshot_id", snapshotId}, {"created_at", savedAt.toString(Qt::ISODateWithMs)},
        {"status", "complete"}, {"archives", archives}, {"entries", entries}};
}

void previous(IncrementalPlanRequest *input, const QJsonObject &root)
{
    input->previousSnapshotId = snapshotId;
    input->previousManifest = QJsonDocument(root).toJson();
    input->lastSuccessfulCheck = savedAt;
}

QStringList paths(const IncrementalPlan &plan, IncrementalPlannedFile::Change change)
{
    QStringList result;
    for (const auto &file : plan.files) if (file.change == change) result.append(file.restorePath);
    return result;
}
}

class IncrementalPlannerTest : public QObject
{
    Q_OBJECT
private slots:
    void firstBaselineAndUnchangedCheck()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("code.cpp");
        QVERIFY(write(path, "hello"));
        QVERIFY(setTime(path));
        auto input = request(directory.path(), {"code.cpp"});
        const auto baseline = IncrementalPlanner::plan(input);
        QVERIFY(baseline.success);
        QVERIFY(baseline.snapshotRequired);
        QCOMPARE(baseline.files.size(), 1);
        QVERIFY(baseline.files[0].requiresUpload());
        QCOMPARE(baseline.files[0].checksum, QCryptographicHash::hash("hello", QCryptographicHash::Sha256));
        QCOMPARE(baseline.files[0].mtimeNs, QString("1700000000123456789"));
        QVERIFY(!baseline.lastSavedChange.isValid());
        const auto root = manifest(baseline);
        QVERIFY(IncrementalManifest::validate(QJsonDocument(root).toJson(), setId, snapshotId));
        previous(&input, root);
        const auto unchanged = IncrementalPlanner::plan(input);
        QVERIFY(unchanged.success);
        QVERIFY(!unchanged.snapshotRequired);
        QCOMPARE(paths(unchanged, IncrementalPlannedFile::Change::Reused), QStringList{"code.cpp"});
        QVERIFY(!unchanged.files[0].requiresUpload());
        QCOMPARE(unchanged.requiredArchives, root.value("archives").toArray());
        QCOMPARE(unchanged.lastSuccessfulCheck, checkedAt);
        QCOMPARE(unchanged.lastSavedChange, savedAt);
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files), QStringList{"code.cpp"});
    }

    void additionsEditsDeletionsAndSelectionChanges()
    {
        QTemporaryDir directory;
        for (const auto &name : {"unchanged", "edited", "removed"}) QVERIFY(write(directory.filePath(name), name));
        auto input = request(directory.path(), {"unchanged", "edited", "removed"});
        const auto old = IncrementalPlanner::plan(input);
        QVERIFY(old.success);
        const auto root = manifest(old);
        QVERIFY(write(directory.filePath("edited"), "new version"));
        QVERIFY(write(directory.filePath("added"), "new file"));
        input.sources = request(directory.path(), {"edited", "added", "unchanged"}).sources;
        previous(&input, root);
        const auto plan = IncrementalPlanner::plan(input);
        QVERIFY(plan.success);
        QVERIFY(plan.snapshotRequired);
        QCOMPARE(paths(plan, IncrementalPlannedFile::Change::Added), QStringList{"added"});
        QCOMPARE(paths(plan, IncrementalPlannedFile::Change::Changed), QStringList{"edited"});
        QCOMPARE(paths(plan, IncrementalPlannedFile::Change::Reused), QStringList{"unchanged"});
        QCOMPARE(plan.removed, QStringList{"removed"}); // Still on disk: omitted by selection.
        QVERIFY(plan.files[0].requiresUpload());
        QVERIFY(plan.files[1].requiresUpload());
        QVERIFY(!plan.files[2].requiresUpload()); // Do not re-upload its archive neighbor.
        QCOMPARE(plan.requiredArchives, root.value("archives").toArray()); // Complete inventory preserved.
        QCOMPARE(plan.lastSavedChange, savedAt); // Planning does not claim publication.
    }

    void renamedAndDuplicateContent()
    {
        QTemporaryDir directory;
        QVERIFY(write(directory.filePath("old"), "same content"));
        auto input = request(directory.path(), {"old"});
        const auto old = IncrementalPlanner::plan(input);
        QVERIFY(old.success);
        QVERIFY(QFile::rename(directory.filePath("old"), directory.filePath("renamed")));
        QVERIFY(write(directory.filePath("duplicate"), "same content"));
        input.sources = request(directory.path(), {"renamed", "duplicate"}).sources;
        previous(&input, manifest(old));
        const auto plan = IncrementalPlanner::plan(input);
        QVERIFY(plan.success);
        QVERIFY(plan.snapshotRequired);
        QCOMPARE(paths(plan, IncrementalPlannedFile::Change::Added), (QStringList{"duplicate", "renamed"}));
        QCOMPARE(plan.removed, QStringList{"old"});
        QCOMPARE(plan.requiredArchives.size(), 1);
        for (const auto &file : plan.files) {
            QVERIFY(!file.requiresUpload());
            QCOMPARE(file.archiveId, objectId);
            QCOMPARE(file.memberPath, QString("member-0"));
        }
    }

    void sameSizeSameMtimeStillHashes()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("file");
        QVERIFY(write(path, "before"));
        QVERIFY(setTime(path));
        auto input = request(directory.path(), {"file"});
        const auto old = IncrementalPlanner::plan(input);
        QVERIFY(old.success);
        QVERIFY(write(path, "after!"));
        QVERIFY(setTime(path));
        previous(&input, manifest(old));
        const auto plan = IncrementalPlanner::plan(input);
        QVERIFY(plan.success);
        QVERIFY(plan.snapshotRequired);
        QCOMPARE(plan.files[0].size, old.files[0].size);
        QCOMPARE(plan.files[0].mtimeNs, old.files[0].mtimeNs);
        QVERIFY(plan.files[0].checksum != old.files[0].checksum);
        QCOMPARE(plan.files[0].change, IncrementalPlannedFile::Change::Changed);
        QVERIFY(plan.files[0].requiresUpload());
    }

    void metadataOnlyChanges_data()
    {
        QTest::addColumn<bool>("mode");
        QTest::newRow("permission bits") << true;
        QTest::newRow("nanosecond mtime") << false;
    }

    void metadataOnlyChanges()
    {
        QFETCH(bool, mode);
        QTemporaryDir directory;
        const auto path = directory.filePath("file");
        QVERIFY(write(path, "content"));
        QVERIFY(::chmod(QFile::encodeName(path).constData(), 0600) == 0);
        QVERIFY(setTime(path));
        auto input = request(directory.path(), {"file"});
        const auto old = IncrementalPlanner::plan(input);
        QVERIFY(old.success);
        if (mode) QVERIFY(::chmod(QFile::encodeName(path).constData(), 0700) == 0);
        else QVERIFY(setTime(path, 1700000000, 123456790));
        previous(&input, manifest(old));
        const auto plan = IncrementalPlanner::plan(input);
        QVERIFY(plan.success);
        QVERIFY(plan.snapshotRequired);
        QCOMPARE(plan.files[0].change, IncrementalPlannedFile::Change::Changed);
        QVERIFY(!plan.files[0].requiresUpload());
        QCOMPARE(plan.files[0].checksum, old.files[0].checksum);
    }

    void emptyStates()
    {
        auto input = request("/unused", {});
        const auto first = IncrementalPlanner::plan(input);
        QVERIFY(first.success);
        QVERIFY(first.snapshotRequired);
        QVERIFY(first.files.isEmpty());
        previous(&input, manifest(first));
        const auto unchanged = IncrementalPlanner::plan(input);
        QVERIFY(unchanged.success);
        QVERIFY(!unchanged.snapshotRequired);
        QCOMPARE(unchanged.lastSuccessfulCheck, checkedAt);
        QTemporaryDir directory;
        QVERIFY(write(directory.filePath("file"), "content"));
        const auto old = IncrementalPlanner::plan(request(directory.path(), {"file"}));
        previous(&input, manifest(old));
        const auto deleted = IncrementalPlanner::plan(input);
        QVERIFY(deleted.success);
        QVERIFY(deleted.snapshotRequired);
        QCOMPARE(deleted.removed, QStringList{"file"});
        QVERIFY(deleted.requiredArchives.isEmpty());
    }

    void deterministicDuplicateChoiceAndOrdering()
    {
        QTemporaryDir directory;
        for (const auto &name : {"z", "a", "m"}) QVERIFY(write(directory.filePath(name), "duplicate"));
        auto input = request(directory.path(), {"z", "a", "m"});
        const auto old = IncrementalPlanner::plan(input);
        QVERIFY(old.success);
        auto root = manifest(old);
        auto archives = root["archives"].toArray();
        auto otherArchive = archives[0].toObject();
        otherArchive["id"] = secondObjectId;
        otherArchive["remote"] = "objects/" + secondObjectId + ".tar.gz";
        archives.prepend(otherArchive);
        root["archives"] = archives;
        auto entries = root["entries"].toArray();
        auto entry = entries[0].toObject();
        entry["archive"] = secondObjectId;
        entries[0] = entry;
        root["entries"] = entries;
        QVERIFY(IncrementalManifest::validate(QJsonDocument(root).toJson(), setId, snapshotId));
        previous(&input, root);
        const auto first = IncrementalPlanner::plan(input);
        QVERIFY(first.success);
        QVERIFY(!first.snapshotRequired);
        QCOMPARE(first.files[0].restorePath, QString("a"));
        QCOMPARE(first.files[1].restorePath, QString("m"));
        QCOMPARE(first.files[2].restorePath, QString("z"));
        for (const auto &file : first.files) {
            QCOMPARE(file.archiveId, objectId);
            QCOMPARE(file.memberPath, QString("member-1")); // Lowest member in lowest used archive.
        }
        std::reverse(input.sources.begin(), input.sources.end());
        QJsonArray reversed;
        for (const auto &value : entries) reversed.prepend(value);
        root["entries"] = reversed;
        root["archives"] = QJsonArray{archives[1], archives[0]};
        previous(&input, root);
        const auto second = IncrementalPlanner::plan(input);
        QVERIFY(second.success);
        QCOMPARE(second.requiredArchives, first.requiredArchives);
        QCOMPARE(second.removed, first.removed);
        for (int index = 0; index < first.files.size(); ++index) {
            QCOMPARE(second.files[index].restorePath, first.files[index].restorePath);
            QCOMPARE(second.files[index].archiveId, first.files[index].archiveId);
            QCOMPARE(second.files[index].memberPath, first.files[index].memberPath);
        }
    }

    void rejectsForeignOrMalformedHistory()
    {
        auto input = request("/unused", {});
        const auto empty = IncrementalPlanner::plan(input);
        auto root = manifest(empty);
        root["set_id"] = "10000000-0000-4000-8000-000000000002";
        previous(&input, root);
        QVERIFY(!IncrementalPlanner::plan(input).success);
        input.previousManifest = "{";
        QVERIFY(!IncrementalPlanner::plan(input).success);
        input.previousManifest.clear();
        QVERIFY(!IncrementalPlanner::plan(input).success); // Not a new baseline if history was expected.
        input.previousSnapshotId.clear();
        input.checkedAt = {};
        QVERIFY(!IncrementalPlanner::plan(input).success);
    }

    void ignoresUnreferencedPhysicalMembers()
    {
        QTemporaryDir directory;
        QVERIFY(write(directory.filePath("kept"), "kept"));
        QVERIFY(write(directory.filePath("removed"), "removed"));
        auto input = request(directory.path(), {"kept", "removed"});
        const auto old = IncrementalPlanner::plan(input);
        QVERIFY(old.success);
        auto root = manifest(old);
        root["entries"] = QJsonArray{root["entries"].toArray()[0]};
        QVERIFY(IncrementalManifest::validate(QJsonDocument(root).toJson(), setId, snapshotId));
        previous(&input, root);
        const auto plan = IncrementalPlanner::plan(input);
        QVERIFY(plan.success);
        QVERIFY(!plan.files[0].requiresUpload());
        QVERIFY(plan.files[1].requiresUpload()); // Inventory is not a historical content-search index.
        QCOMPARE(plan.requiredArchives, root["archives"].toArray());
    }

    void requiredArchiveOrderAndChangedContentReuse()
    {
        QTemporaryDir directory;
        QVERIFY(write(directory.filePath("a"), "alpha"));
        QVERIFY(write(directory.filePath("b"), "beta"));
        QVERIFY(write(directory.filePath("c"), "gamma"));
        auto input = request(directory.path(), {"a", "b", "c"});
        const auto old = IncrementalPlanner::plan(input);
        QVERIFY(old.success);
        auto root = manifest(old);
        auto entries = root["entries"].toArray();
        auto secondEntry = entries[1].toObject();
        secondEntry["archive"] = secondObjectId;
        entries[1] = secondEntry;
        root["entries"] = entries;
        auto archives = root["archives"].toArray();
        auto secondArchive = archives[0].toObject();
        secondArchive["id"] = secondObjectId;
        secondArchive["remote"] = "objects/" + secondObjectId + ".tar.gz";
        archives.prepend(secondArchive);
        root["archives"] = archives;
        previous(&input, root);
        QVERIFY(write(directory.filePath("c"), "beta"));
        const auto plan = IncrementalPlanner::plan(input);
        QVERIFY(plan.success);
        QVERIFY(plan.snapshotRequired);
        QCOMPARE(plan.files[2].change, IncrementalPlannedFile::Change::Changed);
        QVERIFY(!plan.files[2].requiresUpload());
        QCOMPARE(plan.files[2].archiveId, secondObjectId);
        QCOMPARE(plan.requiredArchives.size(), 2);
        QCOMPARE(plan.requiredArchives[0].toObject()["id"].toString(), objectId);
        QCOMPARE(plan.requiredArchives[1].toObject()["id"].toString(), secondObjectId);
    }

    void rejectsUnsafeOrIncompleteSelections()
    {
        QTemporaryDir directory;
        QVERIFY(write(directory.filePath("file"), "content"));
        auto input = request(directory.path(), {"file"});
        input.scanIssues.append({"missing-root", "scan", "Missing source root"});
        const auto incomplete = IncrementalPlanner::plan(input);
        QVERIFY(!incomplete.success);
        QVERIFY(incomplete.files.isEmpty());
        input.scanIssues.clear();
        input.sources.append(input.sources[0]);
        QVERIFY(!IncrementalPlanner::plan(input).success);
        input.sources = {{directory.filePath("file"), "../escape"}};
        QVERIFY(!IncrementalPlanner::plan(input).success);
        input.sources = {{directory.filePath("file"), "file"}, {directory.filePath("file"), "file/child"}};
        QVERIFY(!IncrementalPlanner::plan(input).success);
        input.sources = {{"relative", "file"}};
        QVERIFY(!IncrementalPlanner::plan(input).success);
        input.sources = {{directory.path(), "directory"}};
        QVERIFY(!IncrementalPlanner::plan(input).success);
        QVERIFY(QFile::link(directory.filePath("file"), directory.filePath("link")));
        input.sources = {{directory.filePath("link"), "link"}};
        QVERIFY(!IncrementalPlanner::plan(input).success);
        QVERIFY(QFile::link(directory.path(), directory.filePath("parent-link")));
        input.sources = {{directory.filePath("parent-link/file"), "file"}};
        QVERIFY(!IncrementalPlanner::plan(input).success);
    }

    void sourceFailureIsTransactional_data()
    {
        QTest::addColumn<QString>("failure");
        for (const auto &failure : {"missing", "write", "replace", "truncate", "cancel"})
            QTest::newRow(failure) << QString(failure);
    }

    void sourceFailureIsTransactional()
    {
        QFETCH(QString, failure);
        QTemporaryDir directory;
        const QString path = directory.filePath("z-file");
        QVERIFY(write(directory.filePath("a-first"), "first"));
        QVERIFY(write(path, QByteArray(256 * 1024, 'x')));
        QVERIFY(setTime(path));
        auto input = request(directory.path(), {"a-first", "z-file"});
        const auto old = IncrementalPlanner::plan(input);
        QVERIFY(old.success);
        previous(&input, manifest(old));
        int calls = 0;
        bool mutationSucceeded = true;
        if (failure == "missing") QVERIFY(QFile::remove(path));
        else input.stopped = [&] {
            // a-first: open/read/EOF (3); z-file: open/read (2); then mutate
            // after the first bounded chunk while its original FD remains open.
            if (++calls != 6) return false;
            if (failure == "cancel") return true;
            if (failure == "replace") mutationSucceeded = QFile::rename(path, path + ".old")
                && write(path, QByteArray(256 * 1024, 'x')) && setTime(path);
            if (failure == "write") mutationSucceeded = write(path, QByteArray(256 * 1024, 'y')) && setTime(path);
            if (failure == "truncate") mutationSucceeded = write(path, "short");
            return false;
        };
        const auto plan = IncrementalPlanner::plan(input);
        QVERIFY(mutationSucceeded);
        QVERIFY(!plan.success);
        QVERIFY(!plan.snapshotRequired);
        QVERIFY(plan.files.isEmpty());
        QVERIFY(plan.removed.isEmpty());
        QVERIFY(plan.requiredArchives.isEmpty());
        QCOMPARE(plan.lastSuccessfulCheck, savedAt);
        QCOMPARE(plan.lastSavedChange, savedAt);
        QCOMPARE(plan.issues.size(), 1);
        QCOMPARE(plan.issues[0].path, path);
    }

    void cancellationOfEmptyCheckDoesNotAdvanceHistory()
    {
        auto input = request("/unused", {});
        input.lastSuccessfulCheck = savedAt;
        input.stopped = [] { return true; };
        const auto plan = IncrementalPlanner::plan(input);
        QVERIFY(!plan.success);
        QCOMPARE(plan.lastSuccessfulCheck, savedAt);
    }
};

QTEST_GUILESS_MAIN(IncrementalPlannerTest)
#include "incrementalplanner_test.moc"

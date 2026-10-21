#include "incrementalrepository.h"
#include "incrementalrestore.h"
#include "backupengine.h"
#include "protonprovider.h"
#include "qprocessrunner.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <csignal>
#include <cstdio>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
QString stateName(IncrementalWriteResult::State state)
{
    switch (state) {
    case IncrementalWriteResult::State::Saved: return "saved";
    case IncrementalWriteResult::State::Unchanged: return "unchanged";
    case IncrementalWriteResult::State::Paused: return "paused";
    case IncrementalWriteResult::State::Cancelled: return "cancelled";
    case IncrementalWriteResult::State::Waiting: return "waiting";
    case IncrementalWriteResult::State::Retrying: return "retrying";
    case IncrementalWriteResult::State::Failed: return "failed";
    }
    return "failed";
}
}

// Advanced direct-request entry point; normal workers use the same v4 executor.
int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("Advanced incremental shared-archive executor. Normal workers also use incremental storage. Requires one writer and manual offline ownership handoff; deletion requires exact-proposal confirmation.");
    parser.addHelpOption();
    parser.addOption({"request", "JSON request containing stable identities, selected file mappings and staging limits.", "path"});
    parser.addOption({"proton-binary", "Proton Drive CLI executable.", "path", "proton-drive"});
    parser.addOption({"initialize", "Provision a new empty repository before the first run."});
    parser.addOption({"acknowledge-single-writer", "Confirm no other computer/configuration writes this set; handoff requires disabling the old writer offline."});
    parser.addOption({"cancel", "Durably cancel the named unfinished operation; never deletes objects or an already published snapshot."});
    parser.addOption({"cleanup", "Plan or retry reference-aware retention only. Set retention, optional legacy_root and cleanup_confirmation in the request. Review proposal before confirming."});
    parser.addOption({"check-integrity", "Start or resume a full remote integrity cycle (one hour/10 GiB per invocation by default)."});
    parser.addOption({"cancel-integrity", "Cancel the integrity cycle without certifying coverage; the next invocation restarts it."});
    parser.addOption({"discover", "Read-only discovery below the chosen remote root; no local request or ownership needed.", "remote-root"});
    parser.addOption({"snapshot", "Read-only restore of this exact sets/<id>/snapshots/<id> directory.", "remote-path"});
    parser.addOption({"destination", "Restore destination. Regular files, POSIX modes and nanosecond mtimes only; no ACLs, owners, links or empty directories.", "path"});
    parser.addOption({"select", "Restore only this exact logical path (repeatable); default is every file.", "relative-path"});
    parser.addOption({"takeover", "Manual OFFLINE handoff to a fresh computer UUID. Request must identify the CURRENT owner/epoch; no server-side fencing.", "computer-id"});
    parser.addOption({"confirm-old-writer-disabled", "Confirm old schedules, retries and processes have stopped and old installation access/configuration is disabled permanently."});
    parser.process(application);
    if (int(parser.isSet("discover")) + int(parser.isSet("snapshot")) + int(parser.isSet("takeover"))
             + int(parser.isSet("initialize")) + int(parser.isSet("cancel")) + int(parser.isSet("cleanup")) + int(parser.isSet("check-integrity")) + int(parser.isSet("cancel-integrity")) > 1) {
        std::fprintf(stderr, "Choose one operation.\n"); return 2;
    }
    if (parser.isSet("discover") || parser.isSet("snapshot")) {
        QProcessRunner runner(parser.value("proton-binary"));
        ProtonProvider provider(runner);
        QString error;
        if (parser.isSet("discover")) {
            QVector<RemoteCopy> copies;
            if (!IncrementalRestore::discover(provider, parser.value("discover"), &copies, &error)) {
                std::fprintf(stderr, "%s\n", qPrintable(error)); return 1;
            }
            QJsonArray output;
            for (const auto &copy : copies) output.append(QJsonObject{{"snapshot", copy.rootPath}, {"set_id", copy.setId},
                {"state", copy.status}, {"files", copy.entries.size()}, {"unavailable", QJsonArray::fromStringList(copy.unavailableItems)},
                {"problems", QJsonArray::fromStringList(copy.failedItems)}});
            std::puts(QJsonDocument(output).toJson(QJsonDocument::Compact).constData());
            return 0;
        }
        RemoteCopy copy;
        if (parser.value("destination").isEmpty()
            || !IncrementalRestore::verifyCopy(provider, parser.value("snapshot"), {}, &copy, &error)) {
            std::fprintf(stderr, "A destination and a valid snapshot are required. %s\n", qPrintable(error)); return 1;
        }
        const auto selection = parser.values("select");
        QVector<BackupEntry> selected;
        QSet<QString> found;
        for (const auto &entry : copy.entries) if (selection.isEmpty() || selection.contains(entry.restorePath)) {
            selected.append(entry);
            found.insert(entry.restorePath);
        }
        for (const auto &path : selection) if (!found.contains(path)) {
            std::fprintf(stderr, "Selected file is missing or unavailable: %s\n", qPrintable(path)); return 1;
        }
        if (selection.isEmpty() && !copy.unavailableItems.isEmpty()) {
            std::fprintf(stderr, "Snapshot contains unavailable files; full restore refused.\n"); return 1;
        }
        BackupEngine engine;
        if (selected.isEmpty()) { std::puts("Empty saved state: no files to place."); return 0; }
        const auto result = engine.restoreFiles({selected, parser.value("destination"), copy.rootPath}, provider);
        if (!result.success) { std::fprintf(stderr, "%s\n", qPrintable(result.error)); return 1; }
        std::printf("Restored %d files.\n", result.restoredCount);
        return 0;
    }
    QFile file(parser.value("request"));
    if (!parser.isSet("acknowledge-single-writer") || !file.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "A readable --request file and --acknowledge-single-writer are required.\n");
        return 2;
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    const auto object = document.object();
    IncrementalWriteRequest request;
    request.remoteSetRoot = object.value("remote_set_root").toString();
    request.scan.setId = object.value("set_id").toString();
    request.computerId = object.value("computer_id").toString();
    request.originComputerId = object.value("origin_computer_id").toString();
    request.ownershipEpoch = object.value("ownership_epoch").toInteger(0);
    request.operationId = object.value("operation_id").toString();
    request.snapshotId = object.value("snapshot_id").toString();
    request.stagingDirectory = object.value("staging_directory").toString();
    request.stateDirectory = object.value("state_directory").toString();
    request.stagingBudget = object.value("staging_budget_bytes").toInteger(1000 * 1000 * 1000);
    request.archiveTargetBytes = object.value("archive_target_bytes").toInteger(1000 * 1000 * 1000);
    request.batchFileLimit = object.value("batch_file_limit").toInt(1000);
    request.retention = object.value("retention").toInt(0);
    request.legacyRoot = object.value("legacy_root").toString();
    request.cleanupConfirmation = object.value("cleanup_confirmation").toString();
    request.integrityByteBudget = object.value("integrity_byte_budget").toInteger(10LL * 1024 * 1024 * 1024);
    request.integrityTimeBudgetMs = object.value("integrity_time_budget_ms").toInteger(60LL * 60 * 1000);
    request.scan.checkedAt = QDateTime::currentDateTimeUtc();
    if (!document.isObject() || (!parser.isSet("takeover") && !object.value("sources").isArray())) {
        std::fprintf(stderr, "Malformed request: an explicit sources array is required.\n"); return 2;
    }
    for (const QString &key : {QString("ownership_epoch"), QString("staging_budget_bytes"), QString("archive_target_bytes"), QString("batch_file_limit"), QString("retention"), QString("integrity_byte_budget"), QString("integrity_time_budget_ms")}) {
        if (object.contains(key) && (!object.value(key).isDouble() || object.value(key).toInteger(-1) < 1
            || object.value(key).toDouble() != double(object.value(key).toInteger(-1)))) {
            std::fprintf(stderr, "Invalid positive integer setting: %s\n", qPrintable(key)); return 2;
        }
    }
    for (const auto &value : object.value("sources").toArray()) {
        const auto source = value.toObject();
        if (!value.isObject() || !source.value("source").isString() || !source.value("restore").isString()) {
            std::fprintf(stderr, "Malformed source mapping.\n"); return 2;
        }
        request.scan.sources.append({source.value("source").toString(), source.value("restore").toString()});
    }
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
    request.checkpoint = [&](const QString &phase) {
        if (interrupted) return IncrementalControl::Pause;
        if (parser.isSet("cancel-integrity") && phase.startsWith("integrity-")) return IncrementalControl::Cancel;
        return parser.isSet("cancel") ? IncrementalControl::Cancel : IncrementalControl::Continue;
    };
    QProcessRunner runner(parser.value("proton-binary"));
    runner.setStopRequested([] { return interrupted != 0; });
    ProtonProvider provider(runner);
    if (parser.isSet("check-integrity") || parser.isSet("cancel-integrity")) {
        const auto result = IncrementalRepository::checkIntegrity(request, provider, parser.isSet("check-integrity"));
        std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
        return result.value("status") == "cancelled" || !result.value("cleanup_blocked").toBool(true) ? 0 : 1;
    }
    if (parser.isSet("cleanup")) {
        QJsonObject proposal;
        QString confirmation, error;
        const bool success = IncrementalRepository::cleanup(request, provider, &proposal, &confirmation, &error);
        std::puts(QJsonDocument(QJsonObject{{"success", success}, {"proposal", proposal},
            {"cleanup_confirmation", confirmation}, {"error", error}}).toJson(QJsonDocument::Compact).constData());
        return success ? 0 : 1;
    }
    if (parser.isSet("takeover")) {
        QString error;
        if (!IncrementalRepository::takeover(request, provider, parser.value("takeover"),
                parser.isSet("confirm-old-writer-disabled"), &error)) {
            std::fprintf(stderr, "%s\n", qPrintable(error)); return 1;
        }
        std::printf("Offline handoff verified. New owner: %s; epoch: %lld. Update the writer request explicitly. No schedules were enabled.\n",
            qPrintable(parser.value("takeover")), static_cast<long long>(request.ownershipEpoch + 1));
        return 0;
    }
    if (parser.isSet("initialize")) {
        QString error;
        if (!IncrementalRepository::initialize(request, provider, true, &error)) {
            std::fprintf(stderr, "%s\n", qPrintable(error)); return 1;
        }
        std::puts("Repository initialized in manual-offline single-writer mode.");
        return 0;
    }
    const auto result = IncrementalRepository::execute(request, provider);
    QJsonArray issues;
    for (const auto &issue : result.issues) issues.append(QJsonObject{{"path", issue.path}, {"phase", issue.phase}, {"reason", issue.reason}});
    const QJsonObject output{{"state", stateName(result.state)}, {"manifest_committed", result.manifestCommitted},
        {"manifest", result.manifestPath}, {"last_successful_check", result.lastSuccessfulCheck.toString(Qt::ISODateWithMs)},
        {"last_saved_change", result.lastSavedChange.toString(Qt::ISODateWithMs)},
        {"damaged_objects", QJsonArray::fromStringList(result.damagedObjects)},
        {"affected_snapshots", QJsonArray::fromStringList(result.affectedSnapshots)}, {"issues", issues},
        {"cleanup_needs_attention", result.cleanupNeedsAttention}, {"cleanup_proposal", result.cleanupProposal},
        {"cleanup_confirmation", result.cleanupConfirmation}};
    auto health = output;
    health["integrity"] = result.integrity;
    std::puts(QJsonDocument(health).toJson(QJsonDocument::Compact).constData());
    return result.success() || result.state == IncrementalWriteResult::State::Cancelled ? 0 : 1;
}

#include "incrementalrepository.h"
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

// Explicit development/advanced-user entry point, not the default backup worker.
int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("Incremental shared-archive executor. Requires one writer and manual offline ownership handoff. No automatic retention; rollout is not enabled.");
    parser.addHelpOption();
    parser.addOption({"request", "JSON request containing stable identities, selected file mappings and staging limits.", "path"});
    parser.addOption({"proton-binary", "Proton Drive CLI executable.", "path", "proton-drive"});
    parser.addOption({"initialize", "Provision a new empty repository before the first run."});
    parser.addOption({"acknowledge-single-writer", "Confirm no other computer/configuration writes this set; handoff requires disabling the old writer offline."});
    parser.addOption({"cancel", "Durably cancel the named unfinished operation; never deletes objects or an already published snapshot."});
    parser.process(application);
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
    request.scan.checkedAt = QDateTime::currentDateTimeUtc();
    if (!document.isObject() || !object.value("sources").isArray()) {
        std::fprintf(stderr, "Malformed request: an explicit sources array is required.\n"); return 2;
    }
    for (const QString &key : {QString("ownership_epoch"), QString("staging_budget_bytes"), QString("archive_target_bytes"), QString("batch_file_limit")}) {
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
    request.checkpoint = [&](const QString &) {
        if (interrupted) return IncrementalControl::Pause;
        return parser.isSet("cancel") ? IncrementalControl::Cancel : IncrementalControl::Continue;
    };
    QProcessRunner runner(parser.value("proton-binary"));
    runner.setStopRequested([] { return interrupted != 0; });
    ProtonProvider provider(runner);
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
        {"affected_snapshots", QJsonArray::fromStringList(result.affectedSnapshots)}, {"issues", issues}};
    std::puts(QJsonDocument(output).toJson(QJsonDocument::Compact).constData());
    return result.success() || result.state == IncrementalWriteResult::State::Cancelled ? 0 : 1;
}

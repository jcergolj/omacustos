#pragma once

#include "processrunner.h"
#include <functional>

struct ProcessTimeouts {
    int metadataMilliseconds = 5 * 60 * 1000;
    int transferMilliseconds = 24 * 60 * 60 * 1000;

    static ProcessTimeouts fromEnvironment();
};

class QProcessRunner final : public ProcessRunner
{
public:
    explicit QProcessRunner(QString executable, ProcessTimeouts timeouts = ProcessTimeouts::fromEnvironment());
    ProcessOutput run(const QStringList &arguments) override;
    void setStopRequested(std::function<bool()> callback) { stopRequested = std::move(callback); }

private:
    QString executable;
    ProcessTimeouts timeouts;
    std::function<bool()> stopRequested;
};

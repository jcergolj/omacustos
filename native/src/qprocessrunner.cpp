#include "qprocessrunner.h"

#include <QElapsedTimer>
#include <QProcess>
#include <limits>
#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace {

bool isTransfer(const QStringList &arguments)
{
    return arguments.size() >= 2 && arguments.at(0) == QStringLiteral("filesystem")
        && (arguments.at(1) == QStringLiteral("upload") || arguments.at(1) == QStringLiteral("download"));
}

}

ProcessTimeouts ProcessTimeouts::fromEnvironment()
{
    ProcessTimeouts timeouts;
    bool valid = false;
    const qint64 seconds = qEnvironmentVariable("OMACUSTOS_TRANSFER_TIMEOUT_SECONDS").toLongLong(&valid);
    if (valid && seconds > 0 && seconds <= std::numeric_limits<int>::max() / 1000) {
        timeouts.transferMilliseconds = static_cast<int>(seconds * 1000);
    }
    return timeouts;
}

QProcessRunner::QProcessRunner(QString executable, ProcessTimeouts timeouts)
    : executable(std::move(executable)), timeouts(timeouts)
{
    // Never pass zero or a negative (unbounded) timeout to QProcess.
    this->timeouts.metadataMilliseconds = qMax(1, timeouts.metadataMilliseconds);
    this->timeouts.transferMilliseconds = qMax(1, timeouts.transferMilliseconds);
}

ProcessOutput QProcessRunner::run(const QStringList &arguments)
{
    QProcess process;
    const bool transfer = isTransfer(arguments);
    const int timeout = transfer ? timeouts.transferMilliseconds : timeouts.metadataMilliseconds;
    QElapsedTimer elapsed;
    elapsed.start();
#ifdef Q_OS_LINUX
    const pid_t parent = ::getpid();
    process.setChildProcessModifier([parent] {
        ::setpgid(0, 0);
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (::getppid() != parent) ::_exit(1);
    });
#endif
    if (stopRequested && stopRequested()) return {-1, {}, QStringLiteral("Backup transfer stopped.")};
    process.start(executable, arguments);

    const auto failure = [&](const QString &message) {
        const QString details = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        return ProcessOutput {-1, QString::fromLocal8Bit(process.readAllStandardOutput()),
            details.isEmpty() ? message : message + QStringLiteral("\n") + details};
    };

    if (!process.waitForStarted(qMin(timeout, 30 * 1000))) {
        const QString message = process.error() == QProcess::Timedout
            ? QStringLiteral("Starting %1 timed out.").arg(executable)
            : QStringLiteral("Unable to start %1: %2").arg(executable, process.errorString());
        process.kill();
        process.waitForFinished(5000);
        return failure(message);
    }

    // The CLI has no documented live byte-progress feed. Output is not evidence
    // of progress, so transfers use a configurable, bounded total runtime.
    while (!process.waitForFinished(static_cast<int>(qMin(qint64(100), qMax(qint64(1), timeout - elapsed.elapsed()))))) {
        if (stopRequested && stopRequested()) {
#ifdef Q_OS_LINUX
            ::kill(-pid_t(process.processId()), SIGKILL);
#endif
            process.kill();
            process.waitForFinished(5000);
            return failure(QStringLiteral("Backup transfer stopped; continuation checkpoint preserved."));
        }
        if (process.state() == QProcess::NotRunning) break;
        if (elapsed.elapsed() < timeout) continue;
        if (process.error() == QProcess::Timedout) {
            process.kill();
            process.waitForFinished(5000);
            return failure(QStringLiteral("The %1 %2 timed out after %3 seconds (total runtime limit).")
                .arg(executable, transfer ? QStringLiteral("transfer") : QStringLiteral("command"))
                .arg(timeout / 1000.0));
        }
        if (process.exitStatus() != QProcess::CrashExit) {
            const QString message = QStringLiteral("The %1 command failed: %2").arg(executable, process.errorString());
            process.kill();
            process.waitForFinished(5000);
            return failure(message);
        }
    }

    if (process.exitStatus() == QProcess::CrashExit) {
        return failure(QStringLiteral("The %1 command crashed: %2").arg(executable, process.errorString()));
    }

    QString standardError = QString::fromLocal8Bit(process.readAllStandardError());
    if (process.exitCode() != 0 && standardError.trimmed().isEmpty()) {
        standardError = QStringLiteral("The %1 command failed with exit code %2.")
            .arg(executable).arg(process.exitCode());
    }
    return {
        process.exitCode(),
        QString::fromLocal8Bit(process.readAllStandardOutput()),
        standardError,
    };
}

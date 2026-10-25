#pragma once

#include <QDateTime>
#include <QString>

struct BackupSchedule {
    QString frequency = QStringLiteral("disabled");
    int hour = 2;
    int minute = 0;
    int weekday = 1;
    int dayOfMonth = 1;
    bool paused = false;

    bool enabled() const { return !paused && frequency != QStringLiteral("disabled"); }
};

class BackupScheduleCalculator final
{
public:
    static QDateTime nextRun(const BackupSchedule &schedule, const QDateTime &after);
    static QDateTime dueRun(const BackupSchedule &schedule, const QDateTime &now);
    static bool isDue(const BackupSchedule &schedule, const QDateTime &lastScheduled, const QDateTime &now);
};

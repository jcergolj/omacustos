#include "backupconfig.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

bool validSchedule(const BackupSchedule &schedule)
{
    return (schedule.frequency == QStringLiteral("disabled")
            || schedule.frequency == QStringLiteral("daily")
            || schedule.frequency == QStringLiteral("weekly")
            || schedule.frequency == QStringLiteral("monthly"))
        && schedule.hour >= 0 && schedule.hour <= 23
        && schedule.minute >= 0 && schedule.minute <= 59
        && schedule.weekday >= 1 && schedule.weekday <= 7
        && schedule.dayOfMonth >= 1 && schedule.dayOfMonth <= 31;
}

bool invalidImportField(const QString &field, const QString &reason, QString *error)
{
    if (error != nullptr) {
        *error = QStringLiteral("Cannot import backup sets: %1 %2.").arg(field, reason);
    }
    return false;
}

bool importString(const QJsonValue &value, const QString &field, QString *error)
{
    return (value.isString() && !value.toString().trimmed().isEmpty())
        || invalidImportField(field, QStringLiteral("must be a nonempty string"), error);
}

bool importInteger(const QJsonValue &value, const QString &field, int minimum, int maximum, QString *error)
{
    const double number = value.toDouble();
    return (value.isDouble() && number >= minimum && number <= maximum && std::floor(number) == number)
        || invalidImportField(field, QStringLiteral("must be a whole number from %1 to %2").arg(minimum).arg(maximum), error);
}

bool validateImport(const QJsonObject &document, QString *error)
{
    if (document.value(QStringLiteral("application")).toString() != QStringLiteral("omacustos")) {
        return invalidImportField(QStringLiteral("application"), QStringLiteral("must be \"omacustos\""), error);
    }
    if (!importInteger(document.value(QStringLiteral("version")), QStringLiteral("version"), 1, 1, error)) {
        return false;
    }
    if (!document.value(QStringLiteral("sets")).isArray()) {
        return invalidImportField(QStringLiteral("sets"), QStringLiteral("must be an array of backup-set objects"), error);
    }
    if (document.contains(QStringLiteral("proton_binary"))
        && !importString(document.value(QStringLiteral("proton_binary")), QStringLiteral("proton_binary"), error)) {
        return false;
    }

    QSet<QString> ids;
    const QJsonArray sets = document.value(QStringLiteral("sets")).toArray();
    for (qsizetype index = 0; index < sets.size(); ++index) {
        const QString path = QStringLiteral("sets[%1]").arg(index);
        if (!sets.at(index).isObject()) {
            return invalidImportField(path, QStringLiteral("must be a backup-set object"), error);
        }
        const QJsonObject set = sets.at(index).toObject();
        for (const QString &field : {QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("remote_root")}) {
            if (!importString(set.value(field), path + '.' + field, error)) {
                return false;
            }
        }
        const QString id = set.value(QStringLiteral("id")).toString();
        if (ids.contains(id)) {
            return invalidImportField(path + QStringLiteral(".id"), QStringLiteral("duplicates another set's ID (%1)").arg(id), error);
        }
        ids.insert(id);

        const QJsonValue sources = set.value(QStringLiteral("source_directories"));
        if (!sources.isArray() || sources.toArray().isEmpty()) {
            return invalidImportField(path + QStringLiteral(".source_directories"), QStringLiteral("must be a nonempty array of path strings"), error);
        }
        const QJsonArray sourceArray = sources.toArray();
        for (qsizetype source = 0; source < sourceArray.size(); ++source) {
            if (!importString(sourceArray.at(source), path + QStringLiteral(".source_directories[%1]").arg(source), error)) {
                return false;
            }
        }
        if (set.contains(QStringLiteral("exclusions"))) {
            const QJsonValue exclusions = set.value(QStringLiteral("exclusions"));
            if (!exclusions.isArray()) {
                return invalidImportField(path + QStringLiteral(".exclusions"), QStringLiteral("must be an array of strings"), error);
            }
            const QJsonArray exclusionArray = exclusions.toArray();
            for (qsizetype exclusion = 0; exclusion < exclusionArray.size(); ++exclusion) {
                if (!exclusionArray.at(exclusion).isString()) {
                    return invalidImportField(path + QStringLiteral(".exclusions[%1]").arg(exclusion), QStringLiteral("must be a string"), error);
                }
            }
        }
        if (set.contains(QStringLiteral("schedule"))) {
            if (!set.value(QStringLiteral("schedule")).isObject()) {
                return invalidImportField(path + QStringLiteral(".schedule"), QStringLiteral("must be an object"), error);
            }
            const QJsonObject schedule = set.value(QStringLiteral("schedule")).toObject();
            if (schedule.contains(QStringLiteral("frequency"))) {
                const QJsonValue frequency = schedule.value(QStringLiteral("frequency"));
                if (!frequency.isString() || !QStringList {QStringLiteral("disabled"), QStringLiteral("daily"),
                    QStringLiteral("weekly"), QStringLiteral("monthly")}.contains(frequency.toString())) {
                    return invalidImportField(path + QStringLiteral(".schedule.frequency"), QStringLiteral("must be disabled, daily, weekly, or monthly"), error);
                }
            }
            for (const QString &field : {QStringLiteral("hour"), QStringLiteral("minute"), QStringLiteral("weekday"), QStringLiteral("day_of_month")}) {
                const int minimum = field == QStringLiteral("hour") || field == QStringLiteral("minute") ? 0 : 1;
                const int maximum = field == QStringLiteral("hour") ? 23 : field == QStringLiteral("minute") ? 59
                    : field == QStringLiteral("weekday") ? 7 : 31;
                if (schedule.contains(field) && !importInteger(schedule.value(field), path + QStringLiteral(".schedule.") + field, minimum, maximum, error)) {
                    return false;
                }
            }
        }
        if (set.contains(QStringLiteral("retention"))
            && !importInteger(set.value(QStringLiteral("retention")), path + QStringLiteral(".retention"), 1, std::numeric_limits<int>::max(), error)) {
            return false;
        }
        if (set.contains(QStringLiteral("only_on_ac_power")) && !set.value(QStringLiteral("only_on_ac_power")).isBool()) {
            return invalidImportField(path + QStringLiteral(".only_on_ac_power"), QStringLiteral("must be true or false"), error);
        }
    }
    return true;
}

}

QString BackupSet::remoteFolder(const QString &computerName) const
{
    const auto segment = [](const QString &value) {
        QString result;
        for (const QChar character : value.trimmed()) {
            result.append(character.isLetterOrNumber() || character == '-' || character == '_' || character == '.'
                ? character : QChar('_'));
        }
        return result.isEmpty() ? QStringLiteral("computer") : result;
    };
    return QDir(remoteRoot).filePath(QDir(segment(computerName)).filePath(segment(name)));
}

BackupConfigStore::BackupConfigStore(QString path)
    : path(std::move(path))
{
}

QString BackupConfigStore::filePath() const
{
    return path;
}

bool BackupConfigStore::load(BackupConfig *config, QString *error) const
{
    return loadFile(config, false, error);
}

bool BackupConfigStore::importSets(BackupConfig *config, QString *error) const
{
    return loadFile(config, true, error);
}

bool BackupConfigStore::loadFile(BackupConfig *config, bool setsOnly, QString *error) const
{
    if (config == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("A destination for OmaCustos backup configuration is required.");
        }

        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("The OmaCustos backup configuration could not be opened.");
        }

        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject object = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) {
            *error = !setsOnly ? QStringLiteral("The OmaCustos backup configuration is malformed.")
                : parseError.error != QJsonParseError::NoError
                ? QStringLiteral("Cannot import backup sets: invalid JSON at byte %1: %2.").arg(parseError.offset).arg(parseError.errorString())
                : QStringLiteral("Cannot import backup sets: the file must contain a JSON object.");
        }

        return false;
    }

    if (setsOnly && !validateImport(object, error)) {
        return false;
    }

    config->sets.clear();
    config->protonBinary = object.value(QStringLiteral("proton_binary")).toString(QStringLiteral("proton-drive"));

    if (object.contains(QStringLiteral("sets"))) {
        const QJsonValue setsValue = object.value(QStringLiteral("sets"));
        if (!setsValue.isArray() || config->protonBinary.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("The OmaCustos backup configuration is malformed.");
            }

            return false;
        }

        QSet<QString> setIds;
        for (const QJsonValue &setValue : setsValue.toArray()) {
            if (!setValue.isObject()) {
                if (error != nullptr) {
                    *error = QStringLiteral("The OmaCustos backup configuration contains an invalid backup.");
                }

                return false;
            }

            const QJsonObject setObject = setValue.toObject();
            const QJsonArray sources = setObject.value(QStringLiteral("source_directories")).toArray();
            const QJsonArray exclusions = setObject.value(QStringLiteral("exclusions")).toArray();
            BackupSet set {
                setObject.value(QStringLiteral("id")).toString(),
                setObject.value(QStringLiteral("name")).toString(),
                setObject.value(QStringLiteral("remote_root")).toString(),
            };

            for (const QJsonValue &source : sources) {
                set.sourceDirectories.append(source.toString());
            }
            for (const QJsonValue &exclusion : exclusions) {
                set.exclusions.append(exclusion.toString());
            }

            const QJsonObject schedule = setObject.value(QStringLiteral("schedule")).toObject();
            set.schedule.frequency = schedule.value(QStringLiteral("frequency")).toString(QStringLiteral("disabled"));
            set.schedule.hour = schedule.value(QStringLiteral("hour")).toInt(2);
            set.schedule.minute = schedule.value(QStringLiteral("minute")).toInt(0);
            set.schedule.weekday = schedule.value(QStringLiteral("weekday")).toInt(1);
            set.schedule.dayOfMonth = schedule.value(QStringLiteral("day_of_month")).toInt(1);
            set.retention = qMax(1, setObject.value(QStringLiteral("retention")).toInt(3));
            set.onlyOnAcPower = setObject.value(QStringLiteral("only_on_ac_power")).toBool(false);
            set.stagingDirectory = setObject.value(QStringLiteral("staging_directory")).toString();
            const auto budget = setObject.value(QStringLiteral("staging_budget_bytes"));
            set.stagingBudget = budget.isUndefined() ? 1000 * 1000 * 1000 : budget.toInteger(-1);
            if ((!budget.isUndefined() && (!budget.isDouble() || budget.toDouble() != double(set.stagingBudget)))
                || set.stagingBudget <= 0 || (setObject.contains("staging_directory") && !setObject.value("staging_directory").isString())) {
                if (error) *error = QStringLiteral("The staging budget must be a positive whole number of bytes and the staging directory must be a string.");
                return false;
            }

            if (set.id.trimmed().isEmpty() || set.name.trimmed().isEmpty() || set.remoteRoot.trimmed().isEmpty()
                || set.sourceDirectories.isEmpty()
                || !validSchedule(set.schedule)
                || setIds.contains(set.id)
                || std::any_of(set.sourceDirectories.cbegin(), set.sourceDirectories.cend(), [](const QString &source) {
                    return source.trimmed().isEmpty();
                })) {
                if (error != nullptr) {
                    *error = QStringLiteral("The OmaCustos backup configuration is incomplete.");
                }

                return false;
            }

            setIds.insert(set.id);
            config->sets.append(set);
        }

        return true;
    }

    const QString source = object.value(QStringLiteral("source_directory")).toString();
    const QString remote = object.value(QStringLiteral("remote_root")).toString();
    if (source.isEmpty() || remote.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("The OmaCustos backup configuration is incomplete.");
        }

        return false;
    }

    config->sets = {
        {
            QStringLiteral("default"),
            QStringLiteral("Default backup"),
            remote,
            {source},
            {},
        },
    };

    return true;
}

bool BackupConfigStore::save(const BackupConfig &config, QString *error) const
{
    return saveFile(config, false, error);
}

bool BackupConfigStore::exportSets(const BackupConfig &config, QString *error) const
{
    return saveFile(config, true, error);
}

bool BackupConfigStore::saveFile(const BackupConfig &config, bool setsOnly, QString *error) const
{
    if (!setsOnly && config.protonBinary.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("The OmaCustos backup configuration is incomplete.");
        }

        return false;
    }

    QJsonObject object;
    if (setsOnly) {
        object.insert(QStringLiteral("application"), QStringLiteral("omacustos"));
        object.insert(QStringLiteral("version"), 1);
    } else {
        object.insert(QStringLiteral("proton_binary"), config.protonBinary);
    }

    QJsonArray sets;
    QSet<QString> setIds;
    for (const BackupSet &set : config.sets) {
        if (set.id.trimmed().isEmpty() || set.name.trimmed().isEmpty() || set.remoteRoot.trimmed().isEmpty()
            || set.sourceDirectories.isEmpty() || !validSchedule(set.schedule) || setIds.contains(set.id)
            || set.stagingBudget <= 0) {
            if (error != nullptr) {
                *error = QStringLiteral("The OmaCustos backup configuration is incomplete.");
            }

            return false;
        }
        setIds.insert(set.id);

        QJsonArray sources;
        for (const QString &source : set.sourceDirectories) {
            if (source.trimmed().isEmpty()) {
                if (error != nullptr) {
                    *error = QStringLiteral("The OmaCustos backup configuration is incomplete.");
                }

                return false;
            }
            sources.append(source);
        }

        QJsonArray exclusions;
        for (const QString &exclusion : set.exclusions) {
            exclusions.append(exclusion);
        }

        const QJsonObject schedule {
            {QStringLiteral("frequency"), set.schedule.frequency},
            {QStringLiteral("hour"), set.schedule.hour},
            {QStringLiteral("minute"), set.schedule.minute},
            {QStringLiteral("weekday"), set.schedule.weekday},
            {QStringLiteral("day_of_month"), set.schedule.dayOfMonth},
        };

        sets.append(QJsonObject {
            {QStringLiteral("id"), set.id},
            {QStringLiteral("name"), set.name},
            {QStringLiteral("remote_root"), set.remoteRoot},
            {QStringLiteral("source_directories"), sources},
            {QStringLiteral("exclusions"), exclusions},
            {QStringLiteral("schedule"), schedule},
            {QStringLiteral("retention"), qMax(1, set.retention)},
            {QStringLiteral("only_on_ac_power"), set.onlyOnAcPower},
            {QStringLiteral("staging_directory"), set.stagingDirectory},
            {QStringLiteral("staging_budget_bytes"), set.stagingBudget},
        });
    }
    object.insert(QStringLiteral("sets"), sets);

    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to create the OmaCustos configuration directory.");
        }

        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to write the OmaCustos backup configuration.");
        }

        return false;
    }

    const QByteArray contents = QJsonDocument(object).toJson(QJsonDocument::Indented);

    if (file.write(contents) != contents.size() || !file.commit()) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to finish writing the OmaCustos backup configuration.");
        }

        return false;
    }

    return true;
}

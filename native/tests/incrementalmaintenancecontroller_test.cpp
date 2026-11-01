#include "incrementalmaintenance.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class IncrementalMaintenanceControllerTest final : public QObject
{
    Q_OBJECT
private slots:
    void emptyDeletionSelectionFailsWithoutWork()
    {
        QTemporaryDir directory;
        IncrementalMaintenance controller(directory.filePath("settings.json"));
        QSignalSpy failed(&controller, &IncrementalMaintenance::failed);
        controller.previewDeleteVersions("set", {});
        QCOMPARE(failed.size(), 1);
        QVERIFY(failed.first().first().toString().contains("Select at least one"));
        QVERIFY(!controller.busy());
    }

    void publishesActivityUntilFailure_data()
    {
        QTest::addColumn<QString>("action");
        QTest::addColumn<QString>("message");
        QTest::newRow("delete") << QString("delete") << QString("Preparing backup deletion…");
        QTest::newRow("preview") << QString("preview") << QString("Reviewing old backups and unused data…");
        QTest::newRow("versions") << QString("versions") << QString("Loading saved backup versions…");
        QTest::newRow("takeover") << QString("takeover") << QString("Moving backup responsibility to this computer…");
    }

    void publishesActivityUntilFailure()
    {
        QFETCH(QString, action);
        QFETCH(QString, message);
        QTemporaryDir directory;
        IncrementalMaintenance controller(directory.filePath("settings.json"));
        QSignalSpy failed(&controller, &IncrementalMaintenance::failed);
        QSignalSpy changed(&controller, &IncrementalMaintenance::busyChanged);
        if (action == "delete") controller.previewDelete("missing-set");
        if (action == "preview") controller.previewCleanup("missing-set");
        if (action == "versions") controller.listVersions("missing-set");
        if (action == "takeover") controller.takeover("missing-set", true);
        QVERIFY(controller.busy());
        QCOMPARE(controller.activeSetId(), "missing-set");
        QCOMPARE(controller.activityText(), message);
        QCOMPARE(changed.size(), 1);
        // No configured set: fail locally, without contacting Proton Drive.
        QVERIFY(failed.wait());
        QVERIFY(!controller.busy());
        QVERIFY(controller.activeSetId().isEmpty());
        QVERIFY(controller.activityText().isEmpty());
        QCOMPARE(changed.size(), 2);
    }
};

QTEST_GUILESS_MAIN(IncrementalMaintenanceControllerTest)
#include "incrementalmaintenancecontroller_test.moc"

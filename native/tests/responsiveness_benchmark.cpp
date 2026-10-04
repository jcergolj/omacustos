#include "backupsetcontroller.h"
#include "backuprestorecontroller.h"
#include "backupmanifest.h"
#include "localprovider.h"

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QSet>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QtConcurrentRun>
#include <atomic>
#include <algorithm>

static QObject *findControl(QObject *root, const QString &name, QSet<QObject *> &visited)
{
    if (!root || visited.contains(root)) return nullptr;
    visited.insert(root);
    if (root->objectName() == name) return root;
    for (auto child : root->children())
        if (auto found = findControl(child, name, visited)) return found;
    if (auto item = qobject_cast<QQuickItem *>(root))
        for (auto child : item->childItems())
            if (auto found = findControl(child, name, visited)) return found;
    return nullptr;
}

static QObject *findControl(QObject *root, const QString &name)
{
    QSet<QObject *> visited;
    return findControl(root, name, visited);
}

static QString memoryEvents()
{
    QFile cgroup("/proc/self/cgroup");
    if (!cgroup.open(QIODevice::ReadOnly)) return {};
    for (const auto &line : cgroup.readAll().split('\n')) {
        if (!line.startsWith("0::")) continue;
        QFile events("/sys/fs/cgroup" + QString::fromUtf8(line.mid(3)) + "/memory.events");
        if (events.open(QIODevice::ReadOnly)) return QString::fromUtf8(events.readAll()).trimmed();
    }
    return {};
}

// Opt-in measurement, deliberately separate from deterministic regression tests.
// Uses the real UI/controllers and filesystem engine; unrelated desktop services
// are doubles so this never touches a user's Proton account or systemd units.
class ResponsivenessBenchmark : public QObject
{
    Q_OBJECT
private slots:
    void measure_data()
    {
        QTest::addColumn<QString>("workload");
        for (const auto &name : {"idle", "backup", "restore", "scan"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void measure()
    {
        QFETCH(QString, workload);
        QTemporaryDir home;
        QVERIFY(home.isValid());
        // Optional resident allocation for a separately reported cgroup-pressure
        // scenario. Resource restrictions are not equivalent to target hardware.
        const int pressureMb = qEnvironmentVariableIntValue("OMACUSTOS_BENCHMARK_MEMORY_MB");
        QVERIFY(pressureMb >= 0 && pressureMb <= 4096);
        QByteArray pressure(qsizetype(pressureMb) * 1024 * 1024, 'm');
        const QString sources = home.filePath("sources");
        const QString remote = home.filePath("remote");
        QVERIFY(QDir().mkpath(sources));
        QVERIFY(QDir().mkpath(remote));
        const QByteArray payload(256, 'x');
        QStringList files;
        for (int i = 0; i < 10000; ++i) {
            const QString path = sources + QString("/file-%1.txt").arg(i, 5, 10, QLatin1Char('0'));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(payload), payload.size());
            files.append(path);
        }
        BackupConfig config;
        BackupRunStore runs(home.filePath("omacustos-backup-runs.json"));
        LocalProvider provider(remote);
        BackupEngine engine;
        QString error;
        QString manifest;
        const QString folder = "backups/computer/Backup0";
        QVERIFY2(engine.backup({sources}, folder + "/copy-000", {},
            {"computer", "set-0", "Backup0", "copy-000", QDateTime::currentDateTimeUtc()},
            provider, &manifest, &error), qPrintable(error));
        QDir(QFileInfo(manifest).absolutePath()).removeRecursively();
        for (int set = 0; set < 20; ++set) {
            const QString id = QString("set-%1").arg(set);
            const QString name = QString("Backup%1").arg(set);
            const QString parent = QString("backups/computer/%1").arg(name);
            config.sets.append({id, name, "backups", {sources}, {}});
            runs.ensureSet(id);
            runs.markSuccess(*runs.find(id), QDateTime::currentDateTimeUtc().addSecs(-set));
            runs.find(id)->remoteCopyPath = parent + "/copy-000";
            for (int copy = 0; copy < 100; ++copy)
                QVERIFY(QDir().mkpath(remote + "/" + parent + QString("/copy-%1").arg(copy, 3, 10, QLatin1Char('0'))));
        }
        QVERIFY(runs.save());
        const QString configPath = home.filePath("settings.json");
        QVERIFY(BackupConfigStore(configPath).save(config));
        BackupSetController sets(engine, configPath);
        BackupRestoreController restores(engine, &provider);

        QQmlEngine qml;
        QQmlComponent doubles(&qml, QUrl::fromLocalFile(QStringLiteral(OMACUSTOS_SOURCE_DIR "/tests/DashboardControllers.qml")));
        QScopedPointer<QObject> services(doubles.create());
        QVERIFY2(services, qPrintable(doubles.errorString()));
        for (const char *name : {"backupLauncher", "protonFolderBrowser", "recentBackupCopies", "protonAuth", "themeColors", "resourceUsage", "backupScheduler"})
            qml.rootContext()->setContextProperty(name, services->property(name).value<QObject *>());
        qml.rootContext()->setContextProperty("backupSetController", &sets);
        qml.rootContext()->setContextProperty("restoreController", &restores);
        QQmlComponent component(&qml, QUrl::fromLocalFile(QStringLiteral(OMACUSTOS_SOURCE_DIR "/qml/Main.qml")));
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto window = qobject_cast<QQuickWindow *>(root.data());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTest::qWait(100);

        std::atomic_bool stop = false;
        std::atomic_int passes = 0;
        std::atomic_bool backupSucceeded = true;
        QFuture<void> background;
        if (workload == "backup") {
            QVERIFY(QDir().mkpath(home.filePath("backup-work")));
            background = QtConcurrent::run([&] {
                BackupEngine worker;
                LocalProvider target(home.filePath("backup-work"));
                while (!stop) {
                    QString path, failure;
                    if (!worker.backup({sources}, "copy", {}, target, &path, &failure)) {
                        backupSucceeded = false;
                        break;
                    }
                    QDir(QFileInfo(path).absolutePath()).removeRecursively();
                    ++passes;
                }
            });
        }
        const auto cleanup = qScopeGuard([&] { stop = true; background.waitForFinished(); });
        QVariantList indexes;
        for (int i = 0; i < 10000; ++i) indexes.append(i);
        auto verifySelectedCopy = [&] {
            restores.discover(folder, "set-0");
            QTRY_VERIFY_WITH_TIMEOUT(!restores.busy(), 60000);
            // Copy listing sorts descending; locate the populated copy.
            int index = 0;
            for (const auto &copy : restores.copies()) {
                if (copy.contains("copy-000")) break;
                ++index;
            }
            restores.selectCopy(index);
            QTRY_VERIFY_WITH_TIMEOUT(!restores.busy(), 60000);
            QCOMPARE(restores.entries().size(), 10000);
        };
        auto ensureRestore = [&] {
            if (workload != "restore") return;
            const bool running = restores.property("restoring").isValid()
                ? restores.property("restoring").toBool() : restores.busy();
            if (!running) {
                verifySelectedCopy();
                restores.restoreSelected(indexes, home.filePath("restore-work"));
            }
        };

        QJsonObject results;
        auto item = [&](const char *name) {
            return qobject_cast<QQuickItem *>(findControl(root.data(), QString::fromLatin1(name)));
        };
        auto click = [&](QQuickItem *control) {
            QVERIFY(control);
            const QPoint point = control->mapToScene(QPointF(control->width() / 2, control->height() / 2)).toPoint();
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point, 0);
        };
        auto frame = [&](QElapsedTimer &timer) {
            QSignalSpy swapped(window, &QQuickWindow::frameSwapped);
            window->requestUpdate();
            if (swapped.isEmpty() && !swapped.wait(5000)) return -1.0;
            return timer.nsecsElapsed() / 1000000.0;
        };
        auto report = [&](const QString &interaction, QList<double> timings) {
            if (timings.isEmpty()) return;
            std::sort(timings.begin(), timings.end());
            results[interaction] = QJsonObject {{"samples", timings.size()},
                {"p95_ms", timings.at(qCeil(timings.size() * 0.95) - 1)}, {"worst_ms", timings.last()}};
        };
        const int repetitions = qEnvironmentVariableIntValue("OMACUSTOS_BENCHMARK_SAMPLES") > 0
            ? qEnvironmentVariableIntValue("OMACUSTOS_BENCHMARK_SAMPLES") : 50;
        QList<double> selections;
        int completedDuringSelection = 0;
        verifySelectedCopy();
        root->setProperty("showRestore", true);
        auto list = item("restoreFilesList");
        QVERIFY(list);
        auto scroll = root->findChild<QObject *>("restoreScrollView");
        QVERIFY(scroll);
        auto content = qobject_cast<QQuickItem *>(scroll->property("contentItem").value<QObject *>());
        QVERIFY(content);
        auto dashboardScroll = root->findChild<QObject *>("dashboardScrollView");
        QVERIFY(dashboardScroll);
        auto dashboardContent = root->findChild<QQuickItem *>("dashboardSetsList");
        QVERIFY(dashboardContent);
        content->setProperty("contentY", list->mapToItem(content, QPointF()).y());
        QTest::qWait(50);
        QVariantList selection;
        QStringList selectionPaths;
        const QStringList entries = restores.entries();
        for (int i = 0; i < 100; ++i) {
            selection.append(i);
            selectionPaths.append(entries.at(i));
        }
        root->setProperty("selectedRestoreIndexes", selection);
        root->setProperty("selectedRestorePaths", selectionPaths);
        for (int i = 0; i < repetitions; ++i) {
            ensureRestore();
            root->setProperty("showRestore", true);
            root->setProperty("selectedRestoreIndexes", selection);
            root->setProperty("selectedRestorePaths", selectionPaths);
            content->setProperty("contentY", list->mapToItem(content, QPointF()).y());
            QTest::qWait(10);
            QElapsedTimer timer;
            timer.start();
            if (workload == "scan") sets.preview();
            click(item("restoreFile-0"));
            const double latency = frame(timer);
            QVERIFY(latency >= 0);
            selections.append(latency);
            const int selectedCount = root->property("selectedRestoreIndexes").toList().size();
            if (workload == "restore" && selectedCount == 0 && !restores.busy()) {
                // Successful completion intentionally closes/resets the panel.
                // Report this boundary crossing instead of treating it as a
                // selection regression or silently discarding a slow sample.
                ++completedDuringSelection;
            } else {
                QCOMPARE(selectedCount, 99);
            }
        }
        report("selection_100_ticks_in_10000_files", selections);
        results["restore_completed_during_selection"] = completedDuringSelection;

        QList<double> navigation, typing;
        for (int i = 0; i < repetitions; ++i) {
            ensureRestore();
            root->setProperty("showEditor", false);
            root->setProperty("showRestore", false);
            dashboardContent->setProperty("contentY", 0);
            QTest::qWait(20);
            click(item("setActions-0"));
            QObject *menu = findControl(root.data(), "setMenu-0");
            QVERIFY(menu);
            QTest::qWait(20);
            QQmlExpression expression(qmlContext(menu), menu, "itemAt(0)");
            auto edit = qobject_cast<QQuickItem *>(expression.evaluate().value<QObject *>());
            QElapsedTimer timer;
            timer.start();
            if (workload == "scan") sets.preview();
            click(edit);
            const double navigationLatency = frame(timer);
            QVERIFY(navigationLatency >= 0);
            navigation.append(navigationLatency);
            QVERIFY(root->property("showEditor").toBool());
            auto field = item("setNameField");
            QVERIFY(field);
            QVERIFY(field->isVisible() && field->isEnabled() && field->hasActiveFocus());
            field->forceActiveFocus();
            const int previousLength = field->property("text").toString().size();
            timer.restart();
            if (workload == "scan") sets.preview();
            QTest::keyClick(window, Qt::Key_X, Qt::NoModifier, 0);
            typing.append(frame(timer));
            QVERIFY(typing.last() >= 0);
            QCOMPARE(field->property("text").toString().size(), previousLength + 1);
            QTest::keyClick(window, Qt::Key_Backspace, Qt::NoModifier, 0);
        }
        report("editor_navigation", navigation);
        report("typing", typing);
        {
            QList<double> restoreNavigation;
            int unavailable = 0;
            int usefulFocus = 0;
            QList<double> completedNavigation;
            for (int i = 0; i < repetitions; ++i) {
                root->setProperty("showEditor", false);
                root->setProperty("showRestore", false);
                dashboardContent->setProperty("contentY", 0);
                QTest::qWait(20);
                ensureRestore();
                auto restoreButton = item("restore-0");
                QVERIFY(restoreButton);
                if (!restoreButton->isEnabled()) {
                    ++unavailable;
                    continue;
                }
                QElapsedTimer timer;
                QSignalSpy completed(&restores, &BackupRestoreController::restoreCompleted);
                timer.start();
                if (workload == "scan") sets.preview();
                click(restoreButton);
                const double latency = frame(timer);
                QVERIFY(latency >= 0);
                if (!root->property("showRestore").toBool() && !completed.isEmpty()) {
                    completedNavigation.append(latency);
                    continue;
                }
                QVERIFY(root->property("showRestore").toBool());
                restoreNavigation.append(latency);
                auto focused = window->activeFocusItem();
                if (focused && focused->isEnabled() && focused->hasActiveFocus()) ++usefulFocus;
                if (workload != "restore") QTRY_VERIFY(!restores.busy());
            }
            report("restore_navigation", restoreNavigation);
            report("restore_navigation_completed_at_boundary", completedNavigation);
            results["restore_navigation_unavailable"] = unavailable;
            results["restore_navigation_useful_focus"] = usefulFocus;
        }
        // This backend signal measures Qt frame submission, not compositor
        // presentation or physical display latency. Input starts at test dispatch.
        QJsonObject output {{"workload", workload}, {"metrics", results},
            {"platform", QGuiApplication::platformName()}, {"quick_backend_env", qEnvironmentVariable("QT_QUICK_BACKEND")},
            {"dataset", "20 backups / 100 copies each / 10000 files of 256 bytes"},
            {"backup_passes", passes.load()}, {"pressure_allocation_mb", pressureMb},
            {"fixture_filesystem", QString::fromUtf8(QStorageInfo(home.path()).fileSystemType())},
            {"temporary_parent", QDir::tempPath()}, {"cgroup_memory_events", memoryEvents()},
            {"graphics_api", int(window->rendererInterface()->graphicsApi())},
            {"boundary", "test dispatch to next Qt frameSwapped; scan rows include preview dispatch before input; not display presentation"}};
        qInfo().noquote() << "RESPONSIVENESS" << QJsonDocument(output).toJson(QJsonDocument::Compact);
        QTRY_VERIFY_WITH_TIMEOUT(!restores.busy(), 60000);
        QVERIFY(backupSucceeded);
    }
};

QTEST_MAIN(ResponsivenessBenchmark)
#include "responsiveness_benchmark.moc"

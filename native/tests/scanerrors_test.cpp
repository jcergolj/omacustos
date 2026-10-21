#include "backupengine.h"
#include "localprovider.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QtTest>
#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>

namespace {
QByteArray failingPath;
DIR *failingDirectory = nullptr;
bool failOpen = false;
bool failRead = false;
int deliveredEntries = 0;
}

// Faults are confined to this executable and the exact selected fixture path.
// Qt's iterator otherwise silently turns these OS errors into end-of-directory.
extern "C" DIR *opendir(const char *path)
{
    static auto realOpen = reinterpret_cast<DIR *(*)(const char *)>(dlsym(RTLD_NEXT, "opendir"));
    if (failOpen && failingPath == path) {
        errno = EACCES;
        return nullptr;
    }
    DIR *directory = realOpen(path);
    if (failRead && failingPath == path) {
        failingDirectory = directory;
        deliveredEntries = 0;
    }
    return directory;
}

extern "C" struct dirent *readdir(DIR *directory)
{
    static auto realRead = reinterpret_cast<struct dirent *(*)(DIR *)>(dlsym(RTLD_NEXT, "readdir"));
    if (failRead && directory == failingDirectory && deliveredEntries == 1) {
        errno = EIO;
        return nullptr;
    }
    auto *entry = realRead(directory);
    if (entry && failRead && directory == failingDirectory
        && QByteArray(entry->d_name) != "." && QByteArray(entry->d_name) != "..") ++deliveredEntries;
    return entry;
}

extern "C" int closedir(DIR *directory)
{
    static auto realClose = reinterpret_cast<int (*)(DIR *)>(dlsym(RTLD_NEXT, "closedir"));
    if (directory == failingDirectory) failingDirectory = nullptr;
    return realClose(directory);
}

class ScanErrorsTest : public QObject
{
    Q_OBJECT
private slots:
    void cleanup()
    {
        failOpen = false;
        failRead = false;
        failingDirectory = nullptr;
        failingPath.clear();
        deliveredEntries = 0;
    }

    void enumerationFailure_data()
    {
        QTest::addColumn<bool>("openFailure");
        QTest::addColumn<bool>("boundedArchives");
        QTest::addColumn<bool>("freshCopy");
        QTest::newRow("open-legacy") << true << false << false;
        QTest::newRow("read-legacy") << false << false << false;
        QTest::newRow("open-fresh-legacy") << true << false << true;
        QTest::newRow("read-fresh-legacy") << false << false << true;
        QTest::newRow("open-bounded") << true << true << true;
        QTest::newRow("read-bounded") << false << true << true;
    }

    void enumerationFailure()
    {
        QFETCH(bool, openFailure);
        QFETCH(bool, boundedArchives);
        QFETCH(bool, freshCopy);
        QTemporaryDir temporary(QDir::current().filePath("scan-error-XXXXXX"));
        QVERIFY(temporary.isValid());
        const QString source = temporary.filePath("source");
        const QString failed = source + "/unreadable";
        QVERIFY(QDir().mkpath(failed));
        QFile selected(source + "/selected.txt");
        QVERIFY(selected.open(QIODevice::WriteOnly));
        QCOMPARE(selected.write("keep"), 4);
        selected.close();
        QFile partial(failed + "/partial.txt");
        QVERIFY(partial.open(QIODevice::WriteOnly));
        QCOMPARE(partial.write("partial"), 7);
        partial.close();
        failingPath = QFile::encodeName(failed);
        failOpen = openFailure;
        failRead = !openFailure;

        BackupEngine engine;
        const auto preview = engine.preview({source}, {});
        QVERIFY(preview.includedFiles.contains(selected.fileName()));
        QCOMPARE(preview.includedFiles.contains(partial.fileName()), !openFailure);
        QCOMPARE(preview.scanErrors, QStringList{failed});

        LocalProvider provider(temporary.filePath("provider"));
        BackupOptions options;
        options.freshCopy = freshCopy;
        options.boundedArchives = boundedArchives;
        options.stagingDirectory = temporary.filePath("staging");
        options.continuationDirectory = temporary.filePath("continuation");
        BackupResult result;
        QString manifest, error;
        const QString remote = "backup";
        QVERIFY(!engine.backup({source}, remote, {}, {}, provider, &manifest, &error, {}, &result, options));
        QVERIFY2(error.contains("enumeration", Qt::CaseInsensitive), qPrintable(error));
        QVERIFY(!result.manifestVerified);
        QVERIFY(!QFile::exists(temporary.filePath("provider/backup/manifest.json")));
        QVERIFY(std::any_of(result.issues.cbegin(), result.issues.cend(), [&](const BackupIssue &issue) {
            return issue.path == failed && issue.phase == "selection";
        }));
    }

    void successfulEnumeration()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile visible(temporary.filePath("file.txt"));
        QVERIFY(visible.open(QIODevice::WriteOnly));
        visible.close();
        QFile hidden(temporary.filePath(".hidden"));
        QVERIFY(hidden.open(QIODevice::WriteOnly));
        hidden.close();
        BackupEngine engine;
        const auto preview = engine.preview({temporary.path()}, {});
        QVERIFY(preview.scanErrors.isEmpty());
        QCOMPARE(preview.includedFiles, (QStringList{hidden.fileName(), visible.fileName()}));
    }

    void inaccessibleSelectedDirectory_data()
    {
        QTest::addColumn<bool>("root");
        QTest::addColumn<bool>("searchable");
        QTest::newRow("unreadable-root") << true << true;
        QTest::newRow("unsearchable-root") << true << false;
        QTest::newRow("unreadable-descendant") << false << true;
        QTest::newRow("unsearchable-descendant") << false << false;
    }

    void inaccessibleSelectedDirectory()
    {
        if (::geteuid() == 0) QSKIP("Permission-denial assertions require an unprivileged user.");
        QFETCH(bool, root);
        QFETCH(bool, searchable);
        QTemporaryDir temporary(QDir::current().filePath("scan-permission-XXXXXX"));
        QVERIFY(temporary.isValid());
        const QString source = temporary.filePath("source");
        const QString denied = root ? source : source + "/denied";
        QVERIFY(QDir().mkpath(denied));
        QFile selected(temporary.filePath("selected.txt"));
        QVERIFY(selected.open(QIODevice::WriteOnly));
        QCOMPARE(selected.write("keep"), 4);
        selected.close();
        const auto restorePermissions = qScopeGuard([&] {
            QFile::setPermissions(denied, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        });
        QVERIFY(QFile::setPermissions(denied, searchable ? QFileDevice::ExeOwner : QFileDevice::ReadOwner));
        BackupEngine engine;
        const auto preview = engine.preview({source, selected.fileName()}, {});
        QCOMPARE(preview.scanErrors, QStringList{denied});
        QVERIFY(preview.includedFiles.contains(selected.fileName()));
        for (int mode = 0; mode < 3; ++mode) {
            LocalProvider provider(temporary.filePath("provider-" + QString::number(mode)));
            BackupOptions options;
            options.freshCopy = mode != 0;
            options.boundedArchives = mode == 2;
            options.stagingDirectory = temporary.filePath("staging-" + QString::number(mode));
            options.continuationDirectory = temporary.filePath("continuation-" + QString::number(mode));
            BackupResult result;
            QString manifest, error;
            QVERIFY(!engine.backup({source, selected.fileName()}, "backup", {}, {}, provider,
                &manifest, &error, {}, &result, options));
            QVERIFY2(error.contains("enumeration", Qt::CaseInsensitive), qPrintable(error));
            QVERIFY(!result.manifestVerified);
            QVERIFY(!QFile::exists(temporary.filePath("provider-" + QString::number(mode) + "/backup/manifest.json")));
        }
    }

    void excludedDirectoryFailureIsNotSelected()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString excluded = temporary.filePath("excluded");
        QVERIFY(QDir().mkpath(excluded));
        failingPath = QFile::encodeName(excluded);
        failOpen = true;
        BackupEngine engine;
        const auto preview = engine.preview({temporary.path()}, {excluded});
        QVERIFY(preview.scanErrors.isEmpty());
    }
};

QTEST_GUILESS_MAIN(ScanErrorsTest)
#include "scanerrors_test.moc"

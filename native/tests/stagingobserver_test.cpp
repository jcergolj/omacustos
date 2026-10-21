#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <fcntl.h>
#include <unistd.h>

extern "C" void omacustos_staging_observer_reset(const char *root);
extern "C" void omacustos_staging_observer_sample();
extern "C" long long omacustos_staging_observer_peak();

class StagingObserverTest : public QObject
{
    Q_OBJECT
private slots:
    void writeGrowthDeletionCopyAndReset()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        omacustos_staging_observer_reset(QFile::encodeName(temporary.path()).constData());
        const auto first = temporary.filePath("first");
        const auto second = temporary.filePath("second");
        int fd = ::open(QFile::encodeName(first).constData(), O_CREAT | O_RDWR, 0600);
        QVERIFY(fd >= 0);
        const QByteArray one(1000, 'a');
        QCOMPARE(::write(fd, one.constData(), one.size()), ssize_t(1000));
        QCOMPARE(omacustos_staging_observer_peak(), 1000LL);
        QCOMPARE(::close(fd), 0);
        fd = ::open(QFile::encodeName(second).constData(), O_CREAT | O_RDWR, 0600);
        QVERIFY(fd >= 0);
        const QByteArray half(500, 'b');
        QCOMPARE(::write(fd, half.constData(), half.size()), ssize_t(500));
        QCOMPARE(omacustos_staging_observer_peak(), 1500LL);
        QVERIFY(QFile::remove(first));
        QCOMPARE(::pwrite(fd, one.constData(), one.size(), 0), ssize_t(1000));
        QCOMPARE(omacustos_staging_observer_peak(), 1500LL);
        QCOMPARE(::ftruncate(fd, 600), 0);
        QCOMPARE(::close(fd), 0);
        const auto copied = temporary.filePath("copied");
        QVERIFY(QFile::copy(second, copied));
        omacustos_staging_observer_sample();
        QCOMPARE(omacustos_staging_observer_peak(), 1500LL);
        const auto third = temporary.filePath("third");
        fd = ::open(QFile::encodeName(third).constData(), O_CREAT | O_RDWR, 0600);
        QVERIFY(fd >= 0);
        QCOMPARE(::write(fd, one.constData(), one.size()), ssize_t(1000));
        QCOMPARE(omacustos_staging_observer_peak(), 2200LL);
        QVERIFY(QFile::remove(third));
        // An unlinked but still open private writer continues consuming bytes.
        QCOMPARE(::pwrite(fd, one.constData(), one.size(), 1000), ssize_t(1000));
        QCOMPARE(omacustos_staging_observer_peak(), 3200LL);
        QCOMPARE(::close(fd), 0);
        omacustos_staging_observer_reset(QFile::encodeName(temporary.path()).constData());
        omacustos_staging_observer_sample();
        QCOMPARE(omacustos_staging_observer_peak(), 1200LL);
    }
};
QTEST_GUILESS_MAIN(StagingObserverTest)
#include "stagingobserver_test.moc"

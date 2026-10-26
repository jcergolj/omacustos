import QtQuick
import QtTest
import "../qml/DownloadCost.js" as DownloadCost

TestCase {
    name: "DownloadCost"

    function test_readableSizes_data() {
        return [
            { tag: "zero", bytes: 0, expected: "0 bytes" },
            { tag: "singular", bytes: 1, expected: "1 byte" },
            { tag: "bytes", bytes: 999, expected: "999 bytes" },
            { tag: "kilobytes", bytes: 1500, expected: "1.5 kB" },
            { tag: "reported-size", bytes: 152932000, expected: "153 MB" },
            { tag: "gigabytes", bytes: 2500000000, expected: "2.5 GB" },
            { tag: "terabytes", bytes: 1000000000000, expected: "1 TB" }
        ]
    }

    function test_readableSizes(data) {
        compare(DownloadCost.formatBytes(data.bytes), data.expected)
    }

    function test_archiveCounts() {
        compare(DownloadCost.describe({ bytes: 152932000, archiveCount: 0 }),
                "Required download: 153 MB")
        compare(DownloadCost.describe({ bytes: 152932000, archiveCount: 1 }),
                "Required download: 153 MB · 1 archive (compressed)")
        compare(DownloadCost.describe({ bytes: 152932000, archiveCount: 2 }),
                "Required download: 153 MB · 2 archives (compressed)")
    }
}

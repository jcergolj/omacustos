.pragma library

function formatBytes(bytes) {
    const units = ["bytes", "kB", "MB", "GB", "TB", "PB"]
    let size = Number(bytes) || 0
    let unit = 0
    while (size >= 1000 && unit < units.length - 1) {
        size /= 1000
        ++unit
    }
    if (unit === 0) return size === 1 ? qsTr("1 byte") : qsTr("%1 bytes").arg(size.toFixed(0))
    return "%1 %2".arg(Number(size.toPrecision(3)).toString()).arg(units[unit])
}

function describe(cost) {
    const size = formatBytes(cost.bytes)
    if (cost.archiveCount === 1)
        return qsTr("Required download: %1 · 1 archive (compressed)").arg(size)
    if (cost.archiveCount > 1)
        return qsTr("Required download: %1 · %2 archives (compressed)").arg(size).arg(cost.archiveCount)
    return qsTr("Required download: %1").arg(size)
}

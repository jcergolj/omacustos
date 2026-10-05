import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Frame {
    id: card
    required property var style
    required property int cardIndex
    required property string setId
    required property string setName
    required property var info
    required property var run
    required property bool runActive
    required property bool deleting
    required property string remaining
    required property var progress
    required property bool copyBusy
    signal editRequested()
    signal backupRequested()
    signal pauseRequested()
    signal resumeRequested()
    signal cancelRequested()
    signal restoreRequested()
    signal openFolderRequested()
    signal detailsRequested()
    signal deleteCopyRequested()
    signal removeRequested()
    readonly property bool active: runActive || deleting
    readonly property bool needsAttention: ["incomplete", "failed", "retrying", "waiting", "authentication_required"].indexOf(run.statusCode) >= 0
    readonly property string statusText: deleting ? qsTr("Deleting latest copy…")
        : run.statusCode === "copy_deleted" ? qsTr("Latest copy deleted")
        : !run.statusCode || run.statusCode === "idle" ? qsTr("No backup yet") : run.status
    padding: style.cardPadding
    implicitHeight: content.implicitHeight + topPadding + bottomPadding
    background: Rectangle {
        color: card.style.backgroundColor
        radius: 10
        border.color: card.style.lineColor
    }

    contentItem: ColumnLayout {
        id: content
        spacing: 10
        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 2
                Button {
                    objectName: "setName-" + card.cardIndex
                    text: card.setName
                    flat: true
                    leftPadding: 0
                    rightPadding: 0
                    font.pixelSize: card.style.sectionTitleSize
                    font.weight: Font.Normal
                    Accessible.name: qsTr("Edit %1").arg(card.setName)
                    Layout.maximumWidth: parent.width
                    contentItem: Label {
                        text: card.setName
                        font: parent.font
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: card.editRequested()
                }
                Label {
                    objectName: "setSources-" + card.cardIndex
                    text: card.info.sourceCount === 1 ? qsTr("1 source") : qsTr("%1 sources").arg(card.info.sourceCount || 0)
                    font.pixelSize: card.style.metadataTypeSize
                    color: card.style.mutedColor
                    Layout.fillWidth: true
                }
            }
            ActionButton {
                id: overflow
                style: card.style
                objectName: "setActions-" + card.cardIndex
                text: "⋯"
                Layout.preferredWidth: 40
                Accessible.name: qsTr("Actions for %1").arg(card.setName)
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
                onClicked: menu.open()
                Menu {
                    id: menu
                    objectName: "setMenu-" + card.cardIndex
                    x: overflow.width - width
                    y: overflow.height
                    MenuItem {
                        text: qsTr("Edit settings")
                        onTriggered: card.editRequested()
                    }
                    MenuItem {
                        objectName: "openFolder-" + card.cardIndex
                        text: qsTr("Open latest copy in Proton Drive")
                        enabled: !!card.run.hasLatestCopy && !card.copyBusy && !card.active
                        onTriggered: card.openFolderRequested()
                    }
                    MenuItem {
                        objectName: "viewBackupDetails-" + card.cardIndex
                        text: qsTr("View latest run details")
                        enabled: !!card.run.statusCode && card.run.statusCode !== "idle"
                        onTriggered: card.detailsRequested()
                    }
                    MenuItem {
                        text: qsTr("Delete latest copy")
                        enabled: !!card.run.hasLatestCopy && !card.copyBusy && !card.active
                        onTriggered: card.deleteCopyRequested()
                    }
                    MenuSeparator {}
                    MenuItem {
                        text: qsTr("Delete backup set")
                        onTriggered: card.removeRequested()
                    }
                }
            }
        }

        Label {
            objectName: "setStatus-" + card.cardIndex
            visible: !card.runActive || card.deleting
            text: (!card.active && card.needsAttention ? "! " : !card.active && card.run.statusCode === "success" ? "✓ " : "") + card.statusText
            font.pixelSize: card.style.bodyTypeSize
            font.weight: Font.Normal
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            objectName: "setLastAttempt-" + card.cardIndex
            visible: !card.active && (card.run.lastAttempt || "").length > 0
            text: qsTr("Last attempt: %1").arg(card.run.lastAttempt || "")
            font.pixelSize: card.style.metadataTypeSize
            color: card.style.mutedColor
            Layout.fillWidth: true
        }
        Label {
            objectName: "setWaitingReason-" + card.cardIndex
            visible: !card.active && (card.run.error || "").length > 0
            text: card.run.error || ""
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            objectName: "setResultSummary-" + card.cardIndex
            visible: !card.active && (card.run.summary || "").length > 0
            text: card.run.summary || ""
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            objectName: "setLastSuccess-" + card.cardIndex
            visible: card.run.statusCode !== "success" && (card.run.lastSuccess || "").length > 0
            text: qsTr("Last successful backup: %1").arg(card.run.lastSuccess || "")
            font.pixelSize: card.style.metadataTypeSize
            color: card.style.mutedColor
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        TransferProgress {
            style: card.style
            visible: card.active
            statusText: card.deleting ? qsTr("Deleting backup copy…") : card.remaining
            detailText: card.runActive ? (card.progress.text || "").split("\n")[0] : ""
            fraction: card.deleting ? 0 : card.progress.fraction || 0
            indeterminate: card.deleting || card.progress.indeterminate === undefined || card.progress.indeterminate
            accessibleName: card.deleting ? qsTr("Deleting backup copy") : qsTr("Backup work processed")
            statusObjectName: "setRemainingTime-" + card.cardIndex
            detailObjectName: "setTransferProgress-" + card.cardIndex
            barObjectName: "setProgressBar-" + card.cardIndex
            Layout.fillWidth: true
        }

        Label {
            objectName: "setSchedule-" + card.cardIndex
            text: (card.info.schedule || qsTr("Manual backups"))
                + (card.info.onlyOnAcPower ? qsTr(" · AC power only") : "")
            color: card.style.mutedColor
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            objectName: "setNextRun-" + card.cardIndex
            visible: (card.info.nextRun || "").length > 0
            text: qsTr("Next scheduled backup: %1 (local time)").arg(card.info.nextRun || "")
            font.pixelSize: card.style.metadataTypeSize
            color: card.style.mutedColor
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            objectName: "setCopyPolicy-" + card.cardIndex
            text: card.info.retention === 1 ? qsTr("Retention: keep 1 successful copy. Restore lets you choose an available copy.")
                : qsTr("Retention: keep %1 successful copies. Restore lets you choose an available copy.").arg(card.info.retention || 3)
            font.pixelSize: card.style.metadataTypeSize
            color: card.style.mutedColor
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: card.style.buttonSpacing
            Item { Layout.fillWidth: true }
            ActionButton {
                style: card.style
                objectName: "pauseBackup-" + card.cardIndex
                text: qsTr("Pause")
                visible: card.runActive || (!!card.run.unfinished && card.run.statusCode !== "paused")
                enabled: !(card.run.controlRequest || "").length
                onClicked: card.pauseRequested()
            }
            ActionButton {
                style: card.style
                objectName: "resumeBackup-" + card.cardIndex
                text: qsTr("Resume")
                visible: !card.runActive && (card.run.statusCode === "paused" || !!card.run.unfinished)
                onClicked: card.resumeRequested()
            }
            ActionButton {
                style: card.style
                objectName: "cancelBackup-" + card.cardIndex
                text: qsTr("Cancel")
                visible: card.runActive || !!card.run.unfinished || ["paused", "pending", "waiting", "retrying", "authentication_required"].indexOf(card.run.statusCode) >= 0
                enabled: card.run.controlRequest !== "cancel"
                onClicked: card.cancelRequested()
            }
            ActionButton {
                style: card.style
                objectName: "backUpNow-" + card.cardIndex
                text: qsTr("Back up now")
                visible: !card.run.unfinished && card.run.statusCode !== "paused"
                enabled: !card.active
                onClicked: card.backupRequested()
            }
            ActionButton {
                style: card.style
                objectName: "restore-" + card.cardIndex
                text: qsTr("Restore copies…")
                enabled: !!card.run.hasActivity && !card.deleting
                ToolTip.visible: hovered
                ToolTip.text: enabled ? qsTr("Choose from the available copies, including older backups") : qsTr("Run a backup first to create a copy")
                onClicked: card.restoreRequested()
            }
            ActionButton {
                style: card.style
                objectName: "viewIssues-" + card.cardIndex
                text: qsTr("View issues")
                visible: card.needsAttention && !card.active
                onClicked: card.detailsRequested()
            }
        }
    }
}

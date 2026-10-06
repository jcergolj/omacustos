import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FormCard {
    id: panel
    required property var controller
    signal doneRequested()
    signal selectionRequested()
    signal issuesRequested()
    objectName: "restoreProgressPanel"
    Layout.fillWidth: true
    Layout.bottomMargin: style.contentPadding

    property var focusViewport: null
    property var focusedAction: null

    function ensureFocusedActionVisible() {
        if (!visible || !focusViewport || !focusedAction || !focusedAction.activeFocus) return
        const position = focusedAction.mapToItem(focusViewport, 0, 0)
        const bottom = position.y + focusedAction.height
        if (position.y < 0) focusViewport.contentItem.contentY += position.y
        else if (bottom > focusViewport.height)
            focusViewport.contentItem.contentY += bottom - focusViewport.height
    }

    function focusControls(viewport) {
        focusViewport = viewport
        focusedAction = controller.restoring
            ? (pauseButton.enabled ? pauseButton : stopButton.enabled ? stopButton : null)
            : doneButton
        if (focusedAction) focusedAction.forceActiveFocus()
        else viewport.forceActiveFocus()
        Qt.callLater(ensureFocusedActionVisible)
    }

    onHeightChanged: Qt.callLater(ensureFocusedActionVisible)

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 12

        TransferProgress {
            style: panel.style
            statusObjectName: "activeRestoreProgress"
            detailObjectName: "restoreTransferProgress"
            barObjectName: "restoreProgressBar"
            statusText: {
                switch (panel.controller.restoreState) {
                case "pausing": return qsTr("Pausing restore…")
                case "paused": return qsTr("Restore paused")
                case "stopping": return qsTr("Stopping restore…")
                case "stopped": return qsTr("Restore stopped")
                case "failed": return qsTr("Restore failed")
                case "succeeded": return qsTr("Restore completed")
                default: return qsTr("Restoring %1…").arg(panel.controller.restoreBackupName)
                }
            }
            detailText: panel.controller.restoreProgress
            fraction: panel.controller.restoreProgressFraction
            indeterminate: false
            accessibleName: qsTr("Files restored")
            Layout.fillWidth: true
        }

        Label {
            objectName: "restoreOperationBackup"
            text: qsTr("Backup: %1").arg(panel.controller.restoreBackupName)
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            Layout.fillWidth: true
        }
        Label {
            objectName: "restoreOperationCopy"
            text: qsTr("Copy: %1").arg(panel.controller.restoreCopyLabel || panel.controller.restoreCopyPath)
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            Layout.fillWidth: true
        }
        Label {
            objectName: "restoreOperationDestination"
            text: qsTr("Destination: %1").arg(panel.controller.restoreDestination)
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            Layout.fillWidth: true
        }
        Label {
            objectName: "restoreOperationDownloadCost"
            text: panel.controller.restoreDownloadCost.archiveCount > 0
                ? qsTr("Required download: %1 bytes · %2 archives (compressed)")
                    .arg(panel.controller.restoreDownloadCost.bytes).arg(panel.controller.restoreDownloadCost.archiveCount)
                : qsTr("Required download: %1 bytes").arg(panel.controller.restoreDownloadCost.bytes || 0)
            color: panel.style.mutedColor
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        RowLayout {
            visible: panel.controller.restoring
            Layout.fillWidth: true
            BusyIndicator {
                objectName: "restoreWorkingIndicator"
                running: panel.controller.restoring && panel.controller.restoreState !== "paused"
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
            }
            Label {
                objectName: "restoreWorkingMessage"
                text: panel.controller.restoreState === "paused"
                    ? qsTr("Resume to continue this restore. Closing the application stops it.")
                    : panel.controller.restoreState === "pausing"
                        ? qsTr("Waiting for the current download or file operation to reach a safe pause point.")
                        : panel.controller.restoreState === "stopping"
                            ? qsTr("Stopping safely and cleaning up unfinished work. Restored files will remain.")
                            : qsTr("Downloading, verifying and restoring files. Progress counts completed files and may stay unchanged during a large download.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }
        Label {
            objectName: "restoreResultError"
            visible: panel.controller.restoreError.length > 0
            text: panel.controller.restoreError
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            Layout.fillWidth: true
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
        }
        Label {
            visible: !panel.controller.restoring && panel.controller.restoreState !== "succeeded"
            text: qsTr("Restored files remain in the destination. Back to selection preserves your original selection; starting again restores those files again.")
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Flow {
            Layout.fillWidth: true
            spacing: panel.style.buttonSpacing
            ActionButton {
                id: pauseButton
                style: panel.style
                objectName: "pauseRestoreButton"
                visible: panel.controller.restoring
                text: panel.controller.restoreState === "paused" ? qsTr("Resume") : qsTr("Pause")
                enabled: panel.controller.restoreState === "running" || panel.controller.restoreState === "paused"
                onClicked: {
                    if (panel.controller.restoreState === "paused") panel.controller.resumeRestore()
                    else panel.controller.pauseRestore()
                }
            }
            ActionButton {
                id: stopButton
                style: panel.style
                objectName: "stopRestoreButton"
                visible: panel.controller.restoring
                text: qsTr("Stop restore")
                enabled: panel.controller.restoreState !== "stopping"
                onClicked: panel.controller.stopRestore()
            }
            ActionButton {
                style: panel.style
                objectName: "restoreViewIssuesButton"
                visible: !panel.controller.restoring && panel.controller.restoreIssues.length > 0
                text: qsTr("View issues")
                onClicked: panel.issuesRequested()
            }
            ActionButton {
                style: panel.style
                objectName: "restoreBackToSelectionButton"
                visible: !panel.controller.restoring && panel.controller.restoreState !== "succeeded"
                text: qsTr("Back to selection")
                onClicked: panel.selectionRequested()
            }
            ActionButton {
                id: doneButton
                style: panel.style
                objectName: "restoreDoneButton"
                visible: !panel.controller.restoring
                text: qsTr("Done")
                onClicked: panel.doneRequested()
            }
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: dashboard
    required property var style
    required property var controller
    required property var launcher
    required property var copies
    required property var folderBrowser
    required property var restoreState
    required property real windowHeight
    signal newSetRequested()
    signal editSetRequested(int index)
    signal removeSetRequested(int index)
    signal restoreRequested(string setId)
    signal openFolderRequested(string setId)
    signal detailsRequested(string setId)
    signal cleanupRequested(string setId)
    signal takeoverRequested(string setId)
    signal integrityRequested(string setId)
    readonly property int attentionCount: controller.setIds.filter(function (id) {
        const status = (controller.runSummaries[id] || {}).statusCode
        return ["failed", "incomplete", "retrying", "authentication_required"].indexOf(status) >= 0
            || !!(controller.runSummaries[id] || {}).cleanupNeedsAttention
    }).length
    spacing: 12

    function scrollToTop() { dashboardSetsList.contentY = 0 }

    Label {
        objectName: "backupAttentionSummary"
        visible: dashboard.attentionCount > 0
        text: dashboard.attentionCount === 1 ? qsTr("1 backup set needs attention")
            : qsTr("%1 backup sets need attention").arg(dashboard.attentionCount)
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
        Layout.leftMargin: dashboard.style.contentPadding
        Layout.rightMargin: dashboard.style.contentPadding
    }

    ListView {
        id: dashboardSetsList
        objectName: "dashboardSetsList"
        model: dashboard.controller.setNames
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.leftMargin: dashboard.style.contentPadding
        Layout.rightMargin: dashboard.style.contentPadding
        clip: true
        spacing: 16
        cacheBuffer: height
        ScrollBar.vertical: ScrollBar {}
        footer: Item { height: 12 }

        ColumnLayout {
            visible: dashboardSetsList.count === 0
            width: parent.width
            spacing: 12

            Label {
                objectName: "emptySetsLabel"
                text: qsTr("No backup sets yet. Create a set to choose your files and schedule, or import an existing configuration from the ⋯ menu.")
                color: dashboard.style.mutedColor
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            ActionButton {
                style: dashboard.style
                objectName: "createFirstBackupSetButton"
                text: qsTr("Create your first backup set")
                Layout.alignment: Qt.AlignRight
                onClicked: dashboard.newSetRequested()
            }
        }

        delegate: BackupSetCard {
            required property int index
            required property string modelData
            objectName: "backupSetCard-" + index
            width: dashboardSetsList.width
            cardIndex: index
            setId: dashboard.controller.setIds[index] || ""
            setName: modelData
            style: dashboard.style
            info: dashboard.controller.setSummaries[setId] || ({})
            run: dashboard.controller.runSummaries[setId] || ({})
            runActive: dashboard.controller.runningSetIds.indexOf(setId) >= 0
            deleting: dashboard.copies.deletingSetId === setId
            integrityChecking: typeof incrementalMaintenance !== "undefined"
                && incrementalMaintenance.busy && incrementalMaintenance.integritySetId === setId
            remaining: dashboard.controller.remainingTimes[setId] || qsTr("Estimating time remaining…")
            progress: dashboard.controller.transferProgress[setId] || ({})
            copyBusy: dashboard.copies.busy || dashboard.folderBrowser.busy
                || (typeof incrementalMaintenance !== "undefined" && incrementalMaintenance.busy)
            onEditRequested: dashboard.editSetRequested(index)
            onBackupRequested: dashboard.launcher.startBackup(setId)
            onPauseRequested: dashboard.launcher.pauseBackup(setId)
            onResumeRequested: {
                if (info.importPaused) dashboard.controller.resumeImportedSet(setId)
                else dashboard.launcher.resumeBackup(setId)
            }
            onCancelRequested: dashboard.launcher.cancelBackup(setId)
            onRestoreRequested: dashboard.restoreRequested(setId)
            onOpenFolderRequested: dashboard.openFolderRequested(setId)
            onDetailsRequested: dashboard.detailsRequested(setId)
            onDeleteCopyRequested: dashboard.copies.requestDelete(setId)
            onCleanupRequested: dashboard.cleanupRequested(setId)
            onTakeoverRequested: dashboard.takeoverRequested(setId)
            onIntegrityRequested: dashboard.integrityRequested(setId)
            onRemoveRequested: dashboard.removeSetRequested(index)
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import "LocalPaths.js" as LocalPaths

ApplicationWindow {
    id: root
    visible: true
    width: 960
    height: 640
    minimumWidth: 760
    minimumHeight: 480
    title: qsTr("OmaCustos")
    property bool showEditor: false
    property bool showRestore: false
    property bool showRestoreProgress: false
    property bool closeAfterRestore: false
    onClosing: function(close) {
        if (restoreController.restoring) {
            close.accepted = false
            closeAfterRestore = true
            restoreController.stopRestore()
        }
    }
    property string notificationMessage: ""

    // Keep the window's dashboard/test API while state lives with its presentation.
    property alias selectedRestoreIndexes: restorePanel.selectedIndexes
    property alias selectedRestorePaths: restorePanel.selectedPaths
    property alias selectedRestoreCopyIndex: restorePanel.selectedCopyIndex

    property alias syncingCurrentSet: backupEditor.syncingCurrentSet
    property alias showAdvanced: backupEditor.showAdvanced
    readonly property bool backupRunning: backupEditor.backupRunning
    property alias systemFontFamily: uiStyle.systemFontFamily
    property alias displayFontFamily: uiStyle.displayFontFamily
    property alias bodyFontFamily: uiStyle.bodyFontFamily
    property alias displayTypeSize: uiStyle.displayTypeSize
    property alias pageTitleSize: uiStyle.pageTitleSize
    property alias sectionTitleSize: uiStyle.sectionTitleSize
    property alias bodyTypeSize: uiStyle.bodyTypeSize
    property alias metadataTypeSize: uiStyle.metadataTypeSize
    property alias bodyLeading: uiStyle.bodyLeading
    property alias readableMeasure: uiStyle.readableMeasure
    property alias contentPadding: uiStyle.contentPadding
    property alias cardPadding: uiStyle.cardPadding
    readonly property color backgroundColor: uiStyle.backgroundColor
    readonly property color inkColor: uiStyle.inkColor
    readonly property color mutedColor: uiStyle.mutedColor
    readonly property color lineColor: uiStyle.lineColor
    readonly property color softColor: uiStyle.softColor
    readonly property color accentColor: uiStyle.accentColor

    UiStyle {
        id: uiStyle
        colors: themeColors.colors
    }

    font.family: bodyFontFamily
    font.pixelSize: bodyTypeSize
    font.weight: Font.Normal
    color: backgroundColor
    palette {
        window: root.backgroundColor
        windowText: root.inkColor
        base: root.backgroundColor
        alternateBase: root.softColor
        text: root.inkColor
        button: root.backgroundColor
        buttonText: root.inkColor
        brightText: themeColors.colors.brightText
        highlight: themeColors.colors.highlight
        highlightedText: themeColors.colors.highlightedText
        placeholderText: root.mutedColor
        light: root.softColor
        midlight: root.softColor
        mid: root.lineColor
        dark: root.lineColor
        shadow: root.lineColor
        toolTipBase: root.softColor
        toolTipText: root.inkColor
        accent: root.accentColor
        link: root.accentColor
        linkVisited: root.accentColor
    }

    function loadCurrentSet() {
        backupEditor.loadCurrentSet()
    }

    function localPath(url) {
        return LocalPaths.localPath(url)
    }

    function createNewSet() {
        if (restoreController.restoring) return
        rememberRestoreContext()
        showRestore = false
        backupSetController.discardUnsavedSet()
        backupSetController.addSet()
        showAdvanced = false
        showEditor = true
        Qt.callLater(function () { backupEditor.focusName() })
    }

    function editSet(index) {
        if (restoreController.restoring) return
        rememberRestoreContext()
        showRestore = false
        backupSetController.discardUnsavedSet()
        backupSetController.currentIndex = index
        showEditor = true
        Qt.callLater(function () { backupEditor.focusName() })
    }

    function restoreRecentBackup(index) {
        const setId = backupSetController.recentBackupSetIds[index]
        restoreBackupSet(setId)
    }

    function restoreBackupSet(setId) {
        const setIndex = backupSetController.setIds.indexOf(setId)
        if (setIndex < 0) {
            return
        }

        openRestoreContext(backupSetController.recentBackupFolderPath(setId), setId)
    }

    function setRestoreSelection(indexes) { restorePanel.selection.setIndexes(indexes) }

    function rememberRestoreContext() {
        if (showRestore) {
            restorePanel.screenScrollY = restoreScrollView.contentItem.contentY
            restorePanel.rememberContext()
        }
    }

    function openRestoreContext(folder, setId) {
        if (restoreController.restoring) return
        showRestoreProgress = false
        rememberRestoreContext()
        backupSetController.discardUnsavedSet()
        restorePanel.activateContext(folder, setId)
        showEditor = false
        showRestore = true
        restoreController.discover(folder, setId)
        Qt.callLater(function () {
            if (!root || !root.showRestore || root.showEditor
                || restoreController.backupId !== setId || restoreController.backupFolder !== folder) return
            restoreScrollView.contentItem.contentY = Math.max(0, restorePanel.screenScrollY)
            restorePanel.focusControls(restoreScrollView)
        })
    }

    function openRecentBackupFolder(index) {
        const setId = backupSetController.recentBackupSetIds[index]
        recentBackupCopies.openCopy(setId)
    }

    function requestRemoveSet(index) {
        removeSetDialog.setIndex = index
        removeSetDialog.setId = backupSetController.setIds[index]
        removeSetDialog.setName = backupSetController.setNames[index]
        removeSetDialog.open()
    }

    function setStatus(message) {
        notificationMessage = message
        if (message.length === 0) {
            notificationTimer.stop()
            notificationToast.close()
        } else {
            notificationToast.open()
            notificationTimer.restart()
        }
    }

    Timer {
        id: notificationTimer
        interval: 5000
        onTriggered: notificationToast.close()
    }

    FileDialog {
        id: importSetsDialog
        objectName: "importSetsDialog"
        title: qsTr("Import backup sets")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("OmaCustos backup sets (*.json)")]
        onAccepted: {
            importModeDialog.filePath = root.localPath(selectedFile)
            importModeDialog.open()
        }
    }

    Dialog {
        id: importModeDialog
        objectName: "importModeDialog"
        anchors.centerIn: parent
        property string filePath: ""
        title: qsTr("Import backup sets")
        width: Math.min(root.width - 2 * root.contentPadding, 480)
        modal: true
        standardButtons: Dialog.Cancel

        function importFile(merge) {
            if (backupSetController.importSets(filePath, merge)) {
                root.showEditor = false
                root.loadCurrentSet()
            }
            close()
        }

        contentItem: ColumnLayout {
            spacing: 16
            Label {
                Layout.fillWidth: true
                text: qsTr("Merge keeps other existing sets, adds new sets, and updates sets with matching IDs. Replace removes the current list and uses only the imported sets.\n\nOmaCustos validates the file before changing your settings.")
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: uiStyle.buttonSpacing
                ActionButton {
                    style: uiStyle
                    objectName: "mergeImportButton"
                    text: qsTr("Merge")
                    onClicked: importModeDialog.importFile(true)
                }
                ActionButton {
                    style: uiStyle
                    objectName: "replaceImportButton"
                    text: qsTr("Replace")
                    onClicked: importModeDialog.importFile(false)
                }
            }
        }
    }

    FileDialog {
        id: exportSetsDialog
        objectName: "exportSetsDialog"
        title: qsTr("Export backup sets")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [qsTr("OmaCustos backup sets (*.json)")]
        onAccepted: backupSetController.exportSets(root.localPath(selectedFile))
    }

    FileDialog {
        id: templateDialog
        objectName: "templateDialog"
        title: qsTr("Download backup-set template")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [qsTr("OmaCustos backup sets (*.json)")]
        onAccepted: backupSetController.saveTemplate(root.localPath(selectedFile))
    }

    Popup {
        id: notificationToast
        objectName: "notificationToast"
        parent: Overlay.overlay
        x: root.width - width - root.contentPadding
        y: root.contentPadding
        width: Math.min(360, root.width - 2 * root.contentPadding)
        padding: 16
        closePolicy: Popup.NoAutoClose
        modal: false
        focus: false
        background: Rectangle {
            color: root.softColor
            radius: 8
            border.color: root.lineColor
        }
        contentItem: Label {
            objectName: "notificationMessageLabel"
            text: root.notificationMessage
            wrapMode: Text.WordWrap
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
        }
    }

    function updateDisabledPalette() {
        // Shared palette roles update every color group; apply disabled roles last.
        palette.disabled.text = mutedColor
        palette.disabled.windowText = mutedColor
        palette.disabled.buttonText = mutedColor
        palette.disabled.button = softColor
    }

    Component.onCompleted: {
        updateDisabledPalette()
        showEditor = false
    }

    Dialog {
        id: removeSetDialog
        objectName: "removeSetDialog"
        anchors.centerIn: parent
        property int setIndex: -1
        property string setId: ""
        property string setName: ""
        title: qsTr("Delete backup set")
        width: Math.min(root.width - 2 * root.contentPadding, 420)
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel

        contentItem: Label {
            text: qsTr("Delete the \"%1\" backup set from OmaCustos? Its configuration and schedule will be removed. Copies already stored in Proton Drive will remain.").arg(removeSetDialog.setName)
            font.pixelSize: root.bodyTypeSize
            lineHeight: root.bodyLeading
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.WordWrap
            padding: 16
        }

        onAccepted: {
            const index = backupSetController.setIds.indexOf(setId)
            if (index >= 0) {
                backupSetController.removeSet(index)
            }
            setIndex = -1
            setId = ""
            setName = ""
        }

        onRejected: {
            setIndex = -1
            setId = ""
            setName = ""
        }
        onOpened: standardButton(Dialog.Ok).text = qsTr("Delete")
    }

    Dialog {
        id: deleteCopyDialog
        objectName: "deleteCopyDialog"
        anchors.centerIn: parent
        property string backupName: ""
        property string copyPath: ""
        title: qsTr("Delete backup copy")
        width: Math.min(root.width - 2 * root.contentPadding, 480)
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: Label {
            text: qsTr("Delete this copy of \"%1\" from Proton Drive? The copy and all its files will be moved to Proton Drive Trash. Your backup set and other copies will remain.\n\n%2")
                .arg(deleteCopyDialog.backupName).arg(deleteCopyDialog.copyPath)
            padding: 16
            wrapMode: Text.Wrap
        }
        onOpened: standardButton(Dialog.Ok).text = qsTr("Delete copy")
        onAccepted: recentBackupCopies.confirmDelete()
        onRejected: recentBackupCopies.cancelDelete()
    }

    Dialog {
        id: restoreIssuesDialog
        objectName: "restoreIssuesDialog"
        anchors.centerIn: parent
        title: qsTr("Restore issues")
        width: Math.min(root.width - 2 * root.contentPadding, 640)
        height: Math.min(root.height - 2 * root.contentPadding, 500)
        modal: true
        standardButtons: Dialog.Close
        contentItem: ScrollView {
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: parent.width
                spacing: 12
                Label {
                    text: restoreController.restoreProgress
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
                Repeater {
                    objectName: "restoreIssues"
                    model: restoreController.restoreIssues
                    delegate: ColumnLayout {
                        id: restoreIssueRow
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        spacing: 4
                        Label {
                            objectName: "restoreIssuePath-" + restoreIssueRow.index
                            text: restoreIssueRow.modelData.path
                            textFormat: Text.PlainText
                            wrapMode: Text.WrapAnywhere
                            Layout.fillWidth: true
                        }
                        Label {
                            objectName: "restoreIssueReason-" + restoreIssueRow.index
                            text: qsTr("%1: %2").arg(restoreIssueRow.modelData.phase).arg(restoreIssueRow.modelData.reason)
                            textFormat: Text.PlainText
                            wrapMode: Text.WrapAnywhere
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: backupDetailsDialog
        objectName: "backupDetailsDialog"
        anchors.centerIn: parent
        property string setId: ""
        property var details: ({})
        onOpened: details = backupSetController.backupDetails(setId)

        Connections {
            target: backupSetController
            function onRunStateChanged() {
                if (backupDetailsDialog.opened)
                    backupDetailsDialog.details = backupSetController.backupDetails(backupDetailsDialog.setId)
            }
        }
        title: qsTr("Backup details")
        width: Math.min(root.width - 2 * root.contentPadding, 640)
        height: Math.min(root.height - 2 * root.contentPadding, 500)
        modal: true
        standardButtons: Dialog.Close

        contentItem: ScrollView {
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: parent.width
                spacing: 12
                Label {
                    objectName: "backupDetailsStatus"
                    text: backupDetailsDialog.details.status || ""
                    font.weight: Font.Normal
                    textFormat: Text.PlainText
                }
                Label {
                    objectName: "backupDetailsSummary"
                    text: backupDetailsDialog.details.summary || ""
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Label {
                    objectName: "backupDetailsError"
                    text: backupDetailsDialog.details.error || ""
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                    Layout.fillWidth: true
                }
                Label {
                    objectName: "backupDetailsCurrentFile"
                    text: qsTr("Current file: %1").arg(backupDetailsDialog.details.currentFile || "")
                    visible: (backupDetailsDialog.details.currentFile || "").length > 0
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                    Layout.fillWidth: true
                }
                Label {
                    text: qsTr("Next retry: %1").arg(backupDetailsDialog.details.nextAttempt || "")
                    visible: (backupDetailsDialog.details.nextAttempt || "").length > 0
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Label {
                    text: backupDetailsDialog.details.copyPath || ""
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                    color: root.mutedColor
                    Layout.fillWidth: true
                }
                Repeater {
                    objectName: "backupIssues"
                    model: backupDetailsDialog.details.issues || []
                    delegate: ColumnLayout {
                        id: issueRow
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        spacing: 4
                        Label {
                            objectName: "backupIssuePath-" + issueRow.index
                            text: issueRow.modelData.path
                            textFormat: Text.PlainText
                            font.weight: Font.Normal
                            wrapMode: Text.WrapAnywhere
                            Layout.fillWidth: true
                        }
                        Label {
                            objectName: "backupIssueReason-" + issueRow.index
                            text: qsTr("%1: %2").arg(issueRow.modelData.phase).arg(issueRow.modelData.reason)
                            textFormat: Text.PlainText
                            wrapMode: Text.WrapAnywhere
                            Layout.fillWidth: true
                        }
                    }
                }
                Label {
                    objectName: "cancelledPartialCopies"
                    visible: (backupDetailsDialog.details.cancelledCopies || []).length > 0
                    text: qsTr("Cancelled partial copies (delete explicitly in Proton Drive when no longer needed):\n%1")
                        .arg((backupDetailsDialog.details.cancelledCopies || []).join("\n"))
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                    Layout.fillWidth: true
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: root.contentPadding
            Layout.rightMargin: root.contentPadding
            Layout.topMargin: 24
            Layout.bottomMargin: 16
            spacing: uiStyle.buttonSpacing

            Label {
                text: qsTr("OmaCustos")
                font.family: root.displayFontFamily
                font.pixelSize: root.displayTypeSize
                font.weight: Font.Normal
                font.letterSpacing: 0.4
                color: root.inkColor
                Layout.fillWidth: true
            }

            ActionButton {
                id: backupSetsMenuButton
                style: uiStyle
                objectName: "backupSetsMenuButton"
                visible: !root.showEditor && !root.showRestore && !root.showRestoreProgress && !restoreController.restoring
                text: "⋯"
                Layout.preferredWidth: 40
                Accessible.name: qsTr("Backup set options")
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
                onClicked: backupSetsMenu.open()
                onVisibleChanged: if (!visible) backupSetsMenu.close()

                Menu {
                    id: backupSetsMenu
                    objectName: "backupSetsMenu"
                    x: backupSetsMenuButton.width - width
                    y: backupSetsMenuButton.height

                    MenuItem {
                        objectName: "newBackupSetButton"
                        text: qsTr("New backup set")
                        Accessible.name: text
                        onTriggered: root.createNewSet()
                    }

                    MenuSeparator {}

                    MenuItem {
                        objectName: "importSetsButton"
                        text: qsTr("Import")
                        enabled: !recentBackupCopies.busy
                        onTriggered: importSetsDialog.open()
                    }

                    MenuItem {
                        objectName: "exportSetsButton"
                        text: qsTr("Export")
                        enabled: backupSetController.setNames.length > 0
                        onTriggered: exportSetsDialog.open()
                    }

                    MenuItem {
                        objectName: "downloadTemplateButton"
                        text: qsTr("Download template")
                        onTriggered: templateDialog.open()
                    }
                }
            }
        }

        RowLayout {
            objectName: "protonErrorRow"
            visible: protonAuth.checked && !protonAuth.authenticated && protonAuth.error.length > 0
            Layout.fillWidth: true
            Layout.leftMargin: root.contentPadding
            Layout.rightMargin: root.contentPadding
            Layout.bottomMargin: 12
            spacing: uiStyle.buttonSpacing

            Label {
                objectName: "protonErrorLabel"
                text: protonAuth.error
                color: root.inkColor
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                Accessible.role: Accessible.AlertMessage
                Accessible.name: text
            }

            ActionButton {
                style: uiStyle
                objectName: "protonSignInButton"
                text: qsTr("Sign in to Proton")
                visible: protonAuth.cliAvailable
                enabled: !protonAuth.checking
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Complete sign-in in your browser. Keep the terminal open until it finishes.")
                onClicked: protonAuth.signIn()
            }

            ActionButton {
                style: uiStyle
                objectName: "protonAuthRetryButton"
                text: qsTr("Retry")
                enabled: !protonAuth.checking
                onClicked: protonAuth.refresh()
            }
        }

        RowLayout {
            objectName: "schedulingErrorRow"
            visible: backupScheduler.error.length > 0
            Layout.fillWidth: true
            Layout.leftMargin: root.contentPadding
            Layout.rightMargin: root.contentPadding
            Layout.bottomMargin: 12
            spacing: uiStyle.buttonSpacing

            Label {
                objectName: "schedulingErrorLabel"
                text: backupScheduler.error
                color: root.inkColor
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Accessible.role: Accessible.AlertMessage
                Accessible.name: text
            }

            ActionButton {
                style: uiStyle
                objectName: "enableSchedulingButton"
                text: qsTr("Enable scheduling")
                visible: backupScheduler.hasSchedules && !backupScheduler.ready
                enabled: !backupScheduler.busy && !resourceUsage.busy
                onClicked: backupScheduler.enable()
            }
        }

        Label {
            objectName: "dashboardRefreshError"
            visible: backupSetController.dashboardRefreshError.length > 0
            text: qsTr("Showing last-successful backup information. Refresh failed: %1").arg(backupSetController.dashboardRefreshError)
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            Layout.leftMargin: root.contentPadding
            Layout.rightMargin: root.contentPadding
        }

        StackLayout {
            currentIndex: root.showRestoreProgress || restoreController.restoring ? 3 : root.showEditor ? 1 : root.showRestore ? 2 : 0
            Layout.fillWidth: true
            Layout.fillHeight: true

            Dashboard {
                id: dashboardScrollView
                objectName: "dashboardScrollView"
                Layout.fillWidth: true
                Layout.fillHeight: true
                style: uiStyle
                controller: backupSetController
                launcher: backupLauncher
                copies: recentBackupCopies
                folderBrowser: protonFolderBrowser
                restoreState: restoreController
                windowHeight: root.height
                onNewSetRequested: root.createNewSet()
                onEditSetRequested: function(index) { root.editSet(index) }
                onRemoveSetRequested: function(index) { root.requestRemoveSet(index) }
                onRestoreRequested: function(setId) { root.restoreBackupSet(setId) }
                onOpenFolderRequested: function(setId) { recentBackupCopies.openCopy(setId) }
                onDetailsRequested: function(setId) {
                    backupDetailsDialog.setId = setId
                    backupDetailsDialog.open()
                }
            }

            BackupEditor {
                id: backupEditor
                style: uiStyle
                controller: backupSetController
                resources: resourceUsage
                onSaved: {
                    root.showEditor = false
                    dashboardScrollView.scrollToTop()
                }
                onCloseRequested: {
                    backupSetController.discardUnsavedSet()
                    root.showEditor = false
                }
            }

            ScrollView {
                id: restoreScrollView
                objectName: "restoreScrollView"
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth

                ColumnLayout {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: root.contentPadding
                    anchors.rightMargin: root.contentPadding

                    RestorePanel {
                        id: restorePanel
                        style: uiStyle
                        controller: restoreController
                        onCloseRequested: {
                            if (restoreController.restoring) return
                            root.rememberRestoreContext()
                            root.showRestore = false
                        }
                    }
                }
            }

            ScrollView {
                id: restoreProgressScrollView
                objectName: "restoreProgressScrollView"
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: root.contentPadding
                    anchors.rightMargin: root.contentPadding
                    RestoreProgressPanel {
                        id: restoreProgressPanel
                        style: uiStyle
                        controller: restoreController
                        onIssuesRequested: restoreIssuesDialog.open()
                        onSelectionRequested: {
                            if (restoreController.restoring) return
                            root.showRestoreProgress = false
                            root.showRestore = true
                            Qt.callLater(function () { restorePanel.focusControls(restoreScrollView) })
                        }
                        onDoneRequested: {
                            if (restoreController.restoring) return
                            if (restoreController.restoreState === "succeeded") {
                                restorePanel.reset()
                                delete restorePanel.contextStates[restorePanel.contextKey]
                            } else root.rememberRestoreContext()
                            root.showRestoreProgress = false
                            root.showRestore = false
                            root.showEditor = false
                            dashboardScrollView.scrollToTop()
                        }
                    }
                }
            }
        }
    }

    onActiveChanged: {
        if (active) {
            protonAuth.refresh()
            backupScheduler.refresh()
        }
    }

    Connections {
        target: backupScheduler
        function onFailed(error) { root.setStatus(error) }
    }

    Connections {
        target: resourceUsage
        function onStatusChanged(message) { root.setStatus(message) }
        function onFailed(error) { root.setStatus(error) }
    }

    Connections {
        target: themeColors
        function onColorsChanged() { Qt.callLater(root.updateDisabledPalette) }
    }

    Connections {
        target: protonAuth
        function onFailed(error) { root.setStatus(error) }
    }

    Connections {
        target: backupSetController
        function onStatusChanged(status) { root.setStatus(status) }
        function onFailed(error) { root.setStatus(error) }
    }

    Connections {
        target: backupLauncher
        function onStarted() { root.setStatus("") }
        function onFailed(error) { root.setStatus(error) }
    }

    Connections {
        target: recentBackupCopies
        function onFolderResolved(path) { protonFolderBrowser.openFolder(path) }
        function onDeleteConfirmationReady(name, path) {
            deleteCopyDialog.backupName = name
            deleteCopyDialog.copyPath = path
            deleteCopyDialog.open()
        }
        function onCopyDeleted(setId) { backupSetController.refreshRunState() }
        function onStatusChanged(message) { root.setStatus(message) }
        function onFailed(error) { root.setStatus(error) }
    }

    Connections {
        target: protonFolderBrowser
        function onFolderResolved(url) {
            if (!Qt.openUrlExternally(url)) {
                root.setStatus(qsTr("Unable to open Proton Drive in your browser."))
            }
        }
        function onFailed(error) { root.setStatus(error) }
    }

    Connections {
        target: restoreController
        function onRestoreStarted() {
            root.rememberRestoreContext()
            root.showEditor = false
            root.showRestoreProgress = true
            restoreProgressScrollView.contentItem.contentY = 0
            Qt.callLater(function () { restoreProgressPanel.focusControls(restoreProgressScrollView) })
        }
        function onBusyChanged() {
            if (root.closeAfterRestore && !restoreController.restoring) Qt.callLater(function () { root.close() })
        }
        function onRestoreStateChanged() {
            if (!restoreController.restoring && root.showRestoreProgress)
                Qt.callLater(function () { restoreProgressPanel.focusControls(restoreProgressScrollView) })
        }
        function onStatusChanged(status) { root.setStatus(status) }
        function onFailed(error) { root.setStatus(error) }
    }
}

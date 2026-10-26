import QtQuick
import "DownloadCost.js" as DownloadCost
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import "LocalPaths.js" as LocalPaths

FormCard {
    id: panel
    required property var controller
    readonly property RestoreSelection selection: RestoreSelection { id: selectionState }
    property alias selectedIndexes: selectionState.selectedIndexes
    property alias selectedPaths: selectionState.selectedPaths
    property alias selectedCopyIndex: selectionState.copyIndex
    property alias selectedCopyPath: selectionState.copyPath
    property alias selectionLookup: selectionState.selectionLookup
    property alias selectionRevision: selectionState.selectionRevision
    property alias selectedCount: selectionState.selectedCount
    readonly property var downloadCost: {
        selectionRevision
        controller.entries
        controller.currentCopyPath
        return controller.downloadCost(selectedIndexes || [])
    }
    property var contextStates: ({})
    property string contextKey: ""
    property real screenScrollY: -1
    property bool restoringContext: false
    property alias folderRows: selectionState.folderRows
    property var collapsedFolders: ({})
    readonly property var visibleTreeRows: selection.treeRows.filter(function (row) {
        let parent = row.path.lastIndexOf("/")
        while (parent > 0) {
            if (collapsedFolders[row.path.slice(0, parent)]) return false
            parent = row.path.lastIndexOf("/", parent - 1)
        }
        return true
    })
    signal closeRequested()

    function toggleFolderExpanded(path) {
        const next = Object.assign({}, collapsedFolders)
        if (next[path]) delete next[path]
        else next[path] = true
        collapsedFolders = next
    }

    objectName: "restorePanel"
    Layout.fillWidth: true
    Layout.bottomMargin: style.contentPadding

    function reset() {
        selection.reset()
        collapsedFolders = ({})
        destinationField.text = ""
        restoreList.contentY = 0
        screenScrollY = -1
    }

    function rememberContext() {
        if (contextKey.length === 0) return
        contextStates[contextKey] = {
            selection: selection.snapshot(),
            collapsedFolders: collapsedFolders,
            destination: destinationField.text, fileScroll: restoreList.contentY,
            screenScroll: screenScrollY
        }
    }

    function activateContext(folder, setId) {
        const key = JSON.stringify([folder, setId])
        rememberContext()
        if (contextKey === key) return
        restoringContext = true
        contextKey = key
        reset()
        const state = contextStates[key]
        if (state) {
            selection.restoreSnapshot(state.selection)
            collapsedFolders = state.collapsedFolders || ({})
            destinationField.text = state.destination
            screenScrollY = state.screenScroll
            Qt.callLater(function () {
                if (panel && panel.contextKey === key) {
                    restoreList.contentY = state.fileScroll
                }
            })
        }
        restoringContext = false
    }

    function folderCheckState(path) {
        return selection.folderCheckState(path)
    }

    function selectFolder(path, checked) {
        selection.selectFolder(path, checked)
    }

    function toggleSelection(index, path, checked) {
        selection.toggleFile(index, checked)
    }

    function focusControls(viewport) {
        function inView(control) {
            if (!control.visible || !control.enabled) return false
            const position = control.mapToItem(viewport, 0, 0)
            return position.y >= 0 && position.y + control.height <= viewport.height
        }
        function firstVisibleControl(item) {
            if (item.activeFocusOnTab && inView(item)) return item
            for (const child of item.children || []) {
                const control = firstVisibleControl(child)
                if (control) return control
            }
            return null
        }
        const target = inView(remoteCopySelector) ? remoteCopySelector : firstVisibleControl(panel)
        if (target) target.forceActiveFocus()
        else viewport.forceActiveFocus()
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 20

        Label {
            objectName: "restoreTitle"
            text: qsTr("Restore")
            font.pixelSize: panel.style.sectionTitleSize
            Layout.fillWidth: true
        }

        Label {
            objectName: "restoreInstructions"
            text: qsTr("Choose a backup copy and a destination folder. All available files are selected by default. Untick individual files or folders to leave them out, then press Start restore below.")
            font.pixelSize: panel.style.bodyTypeSize
            lineHeight: panel.style.bodyLeading
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        ComboBox {
            id: remoteCopySelector
            objectName: "restoreCopySelector"
            model: panel.controller.copies
            currentIndex: panel.controller.currentCopyIndex
            enabled: count > 0
            Layout.fillWidth: true
            // ComboBox defaults to row zero when its model grows. Reapply the
            // controller binding so initial listing keeps an explicit choice,
            // and refreshed/cached selections continue to follow copy identity.
            onModelChanged: currentIndex = Qt.binding(function () { return panel.controller.currentCopyIndex })
            onActivated: {
                panel.controller.selectCopy(currentIndex)
            }
            displayText: currentIndex < 0 ? qsTr("Choose a backup copy…") : currentText
        }

        ActionButton {
            style: panel.style
            objectName: "refreshRestoreCopiesButton"
            text: qsTr("Refresh copies")
            enabled: !panel.controller.busy
            onClicked: panel.controller.discover(panel.controller.backupFolder, panel.controller.backupId)
        }

        Label {
            objectName: "restoreCopyCount"
            visible: !panel.controller.busy && panel.controller.copies.length > 0
            text: panel.controller.copySearch.trim().length > 0
                ? (panel.controller.copies.length === 1 ? qsTr("1 matching copy") : qsTr("%1 matching copies").arg(panel.controller.copies.length))
                : panel.controller.hasUnverifiedCopies
                    ? (panel.controller.copies.length === 1 ? qsTr("1 listed restore point — select it to verify")
                        : qsTr("%1 listed restore points — select one to verify").arg(panel.controller.copies.length))
                : (panel.controller.copies.length === 1 ? qsTr("1 available copy — choose it to restore")
                    : qsTr("%1 available copies — choose the date you want to restore").arg(panel.controller.copies.length))
            color: panel.style.mutedColor
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Label {
            objectName: "restoreUnverifiedCopiesMessage"
            visible: panel.controller.hasUnverifiedCopies
            text: qsTr("Dates are shown in local time. Before verification, they are remote folder dates, not verified backup dates. Select a restore point to validate its metadata and load available files. Incomplete or unavailable points cannot be restored.")
            color: panel.style.mutedColor
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Label {
            objectName: "restoreNoCopiesMessage"
            visible: !panel.controller.busy && panel.controller.copies.length === 0
                && panel.controller.browseError.length === 0
            text: panel.controller.copySearch.trim().length > 0
                ? qsTr("No backup copies match your search.") : qsTr("No backup copies found for this set.")
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        RowLayout {
            visible: panel.controller.browsing
            Layout.fillWidth: true

            BusyIndicator {
                objectName: "restoreLoadingIndicator"
                running: panel.controller.browsing
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
            }

            Label {
                objectName: "restoreLoadingMessage"
                text: panel.controller.loadingMessage
                color: panel.style.mutedColor
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        Label {
            objectName: "restoreRefreshError"
            text: qsTr("Refresh failed: %1").arg(panel.controller.browseError)
            visible: panel.controller.browseError.length > 0
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Label {
            objectName: "restoreCachedDataMessage"
            text: panel.controller.busy
                ? qsTr("Showing the last successful result while refreshing. Cached files cannot be restored until verification finishes.")
                : qsTr("Showing cached data from the last successful refresh. Select the copy to verify it again.")
            visible: panel.controller.showingCachedData
            color: panel.style.mutedColor
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Label {
            text: qsTr("Unavailable or failed items: %1").arg(panel.controller.unavailableEntries.length)
            font.pixelSize: panel.style.metadataTypeSize
            visible: panel.controller.unavailableEntries.length > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        ListView {
            model: panel.controller.unavailableEntries
            visible: panel.controller.unavailableEntries.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(100, contentHeight)
            clip: true
            delegate: Label {
                required property string modelData
                text: modelData
                width: parent ? parent.width : 0
                elide: Text.ElideMiddle
            }
        }

        Label {
            objectName: "restoreFilesHeading"
            text: qsTr("1. Choose files and folders")
            font.family: panel.style.bodyFontFamily
            font.pixelSize: panel.style.sectionTitleSize
            font.weight: Font.Normal
            color: panel.style.accentColor
            Layout.fillWidth: true
        }

        CheckBox {
            objectName: "restoreSelectAllFiles"
            text: qsTr("Select all files")
            visible: panel.controller.entries.length > 0
            Layout.fillWidth: true
            checkState: panel.selectedCount === 0 ? Qt.Unchecked
                : panel.selectedCount === panel.controller.entries.length ? Qt.Checked : Qt.PartiallyChecked
            nextCheckState: function () { return checkState === Qt.Checked ? Qt.Unchecked : Qt.Checked }
            onClicked: {
                panel.selectFolder("", checkState === Qt.Checked)
            }
        }

        ListView {
            id: restoreList
            objectName: "restoreFilesList"
            model: panel.controller.entries.length > 0 ? panel.visibleTreeRows : []
            Layout.fillWidth: true
            Layout.preferredHeight: count > 0 ? Math.max(48, Math.min(300, contentHeight)) : 0
            clip: true
            ScrollBar.vertical: ScrollBar {}
            delegate: CheckBox {
                id: treeRow
                required property int index
                required property var modelData
                objectName: modelData.folder ? "restoreFolder-" + panel.folderRows.indexOf(modelData.path)
                    : "restoreFile-" + modelData.fileIndex
                text: modelData.name
                leftPadding: 6 + modelData.depth * 20
                rightPadding: 10
                topPadding: 7
                bottomPadding: 7
                spacing: 8
                hoverEnabled: true
                width: restoreList.width
                contentItem: RowLayout {
                    spacing: treeRow.spacing
                    Item {
                        Layout.preferredWidth: treeRow.indicator.width
                        Layout.preferredHeight: 1
                    }
                    ToolButton {
                        objectName: "restoreFolderExpander"
                        visible: treeRow.modelData.folder
                        text: panel.collapsedFolders[treeRow.modelData.path] ? "▸" : "▾"
                        font.pixelSize: 16
                        Layout.preferredWidth: 24
                        Layout.preferredHeight: 24
                        Accessible.name: panel.collapsedFolders[treeRow.modelData.path]
                            ? qsTr("Expand %1").arg(treeRow.modelData.name)
                            : qsTr("Collapse %1").arg(treeRow.modelData.name)
                        onClicked: panel.toggleFolderExpanded(treeRow.modelData.path)
                    }
                    Item {
                        visible: !treeRow.modelData.folder
                        Layout.preferredWidth: 24
                        Layout.preferredHeight: 1
                    }
                    Canvas {
                        objectName: "restoreEntryIcon"
                        readonly property bool folder: treeRow.modelData.folder
                        readonly property color ink: folder ? panel.style.accentColor : panel.style.mutedColor
                        Layout.preferredWidth: 20
                        Layout.preferredHeight: 20
                        onInkChanged: requestPaint()
                        onFolderChanged: requestPaint()
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            ctx.strokeStyle = ink
                            ctx.fillStyle = ink
                            ctx.lineWidth = 1.5
                            ctx.lineJoin = "round"
                            ctx.beginPath()
                            if (folder) {
                                ctx.moveTo(2, 5)
                                ctx.lineTo(8, 5)
                                ctx.lineTo(10, 7)
                                ctx.lineTo(18, 7)
                                ctx.lineTo(18, 16)
                                ctx.lineTo(2, 16)
                                ctx.closePath()
                                ctx.globalAlpha = 0.18
                                ctx.fill()
                                ctx.globalAlpha = 1
                            } else {
                                ctx.moveTo(5, 2)
                                ctx.lineTo(12, 2)
                                ctx.lineTo(16, 6)
                                ctx.lineTo(16, 18)
                                ctx.lineTo(5, 18)
                                ctx.closePath()
                                ctx.moveTo(12, 2)
                                ctx.lineTo(12, 6)
                                ctx.lineTo(16, 6)
                            }
                            ctx.stroke()
                        }
                    }
                    Label {
                        objectName: "restoreEntryLabel"
                        text: treeRow.text
                        font.family: panel.style.bodyFontFamily
                        font.pixelSize: panel.style.bodyTypeSize
                        font.weight: treeRow.modelData.folder ? Font.DemiBold : Font.Normal
                        color: panel.style.inkColor
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                }
                background: Rectangle {
                    radius: 5
                    color: panel.style.softColor
                    opacity: treeRow.hovered || treeRow.activeFocus ? 1 : treeRow.modelData.folder ? 0.45 : 0
                    border.width: treeRow.activeFocus ? 1 : 0
                    border.color: panel.style.accentColor
                }
                ToolTip.visible: hovered
                ToolTip.delay: 700
                ToolTip.text: modelData.path
                Accessible.name: modelData.folder
                    ? qsTr("Select all files in %1 and its subfolders").arg(modelData.path) : modelData.path
                checkState: {
                    panel.selectionRevision
                    return modelData.folder ? panel.folderCheckState(modelData.path)
                        : panel.selectionLookup[modelData.fileIndex] !== undefined ? Qt.Checked : Qt.Unchecked
                }
                nextCheckState: function () { return checkState === Qt.Checked ? Qt.Unchecked : Qt.Checked }
                onClicked: {
                    if (modelData.folder) panel.selectFolder(modelData.path, checkState === Qt.Checked)
                    else panel.toggleSelection(modelData.fileIndex, modelData.path, checked)
                }
                onToggled: {
                    if (!modelData.folder) panel.toggleSelection(modelData.fileIndex, modelData.path, checked)
                }
            }
        }

        Label {
            text: remoteCopySelector.currentIndex < 0
                ? qsTr("Choose a backup copy to see the files available to restore.")
                : qsTr("This copy has no verified files available to restore.")
            visible: restoreList.count === 0 && !panel.controller.busy
            color: panel.style.mutedColor
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Label {
            objectName: "restoreSelectionCount"
            text: qsTr("Selected files: %1").arg(panel.selectedCount)
            color: panel.style.mutedColor
            Layout.fillWidth: true
        }

        Label {
            objectName: "restoreDownloadCost"
            text: DownloadCost.describe(panel.downloadCost)
            color: panel.style.mutedColor
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Label {
            objectName: "restoreDestinationHeading"
            text: qsTr("2. Choose the destination folder")
            font.pixelSize: panel.style.sectionTitleSize
            font.weight: Font.Normal
            color: panel.style.accentColor
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: panel.style.buttonSpacing

            TextField {
                id: destinationField
                objectName: "restoreDestinationField"
                placeholderText: qsTr("Choose a folder or enter its full path")
                Accessible.name: qsTr("Restore destination folder")
                Layout.fillWidth: true
            }

            ActionButton {
                style: panel.style
                objectName: "chooseRestoreDestinationButton"
                text: qsTr("Choose folder…")
                onClicked: restoreDestinationDialog.open()
            }
        }

        FolderDialog {
            id: restoreDestinationDialog
            objectName: "restoreDestinationDialog"
            title: qsTr("Choose where to restore the selected files")
            onAccepted: destinationField.text = LocalPaths.localPath(selectedFolder)
        }

        Label {
            text: qsTr("The selected files will be restored into this folder, keeping their backed-up folder structure.")
            font.pixelSize: panel.style.metadataTypeSize
            color: panel.style.mutedColor
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        RowLayout {
            objectName: "restoreActionsRow"
            Layout.fillWidth: true
            spacing: panel.style.buttonSpacing
            Item { Layout.fillWidth: true }
            ActionButton {
                style: panel.style
                objectName: "closeRestoreButton"
                text: qsTr("Cancel")
                Accessible.name: qsTr("Cancel restore")
                onClicked: panel.closeRequested()
            }
            ActionButton {
                style: panel.style
                objectName: "startRestoreButton"
                text: qsTr("Start restore")
                enabled: panel.controller.restoreEligible && panel.selectedCount > 0 && destinationField.text.trim().length > 0
                onClicked: panel.controller.restoreSelected(panel.selectedIndexes, destinationField.text.trim())
            }
        }
    }

    Connections {
        target: panel.controller
        function onEntriesChanged() {
            if (panel.restoringContext) return
            panel.selection.reconcileEntries(panel.controller.entries, panel.controller.browsing)
        }
        function onCurrentCopyIndexChanged() {
            if (panel.restoringContext) return
            panel.selection.syncCopy(panel.controller.currentCopyPath,
                panel.controller.currentCopyIndex, panel.controller.browsing)
        }
    }
}

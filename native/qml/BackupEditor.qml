import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import "LocalPaths.js" as LocalPaths

ScrollView {
    id: editor
    objectName: "editorScrollView"
    required property var style
    required property var controller
    required property var resources
    property bool showAdvanced: false
    property bool syncingCurrentSet: false
    property string loadedSetId: ""
    property var scrollPositions: ({})
    property var advancedStates: ({})
    readonly property bool backupRunning: controller.currentRunStatus === "running"
    signal saved()
    signal closeRequested()

    Layout.fillWidth: true
    Layout.fillHeight: true
    contentWidth: availableWidth
    contentHeight: editorCard.implicitHeight + style.contentPadding

    ListModel { id: sourceModel }

    function lines(value) {
        return value.split(/\r?\n/).map(function (line) {
            return line.trim()
        }).filter(function (line) {
            return line.length > 0
        })
    }

    function loadCurrentSet() {
        const changingSet = loadedSetId !== controller.currentId
        if (changingSet) {
            scrollPositions[loadedSetId] = contentItem.contentY
            advancedStates[loadedSetId] = showAdvanced
            loadedSetId = controller.currentId
            showAdvanced = advancedStates[loadedSetId] || false
        }
        setNameField.text = controller.currentName
        remoteField.text = controller.currentRemoteRoot
        sourceModel.clear()
        controller.currentSources.forEach(function (source) {
            sourceModel.append({ path: source })
        })
        exclusionsField.text = controller.currentExclusions.join("\n")
        scheduleFrequency.currentIndex = scheduleFrequency.model.indexOf(controller.currentScheduleFrequency)
        scheduleTimeField.text = "%1:%2".arg(controller.currentScheduleHour.toString().padStart(2, "0"))
            .arg(controller.currentScheduleMinute.toString().padStart(2, "0"))
        scheduleWeekday.currentIndex = controller.currentScheduleWeekday - 1
        scheduleDay.value = controller.currentScheduleDayOfMonth
        retentionSpin.value = controller.currentRetention
        acPowerCheck.checked = controller.currentOnlyOnAcPower
        loadResourcePreset()
        if (changingSet) {
            const id = loadedSetId
            Qt.callLater(function () {
                if (editor && editor.loadedSetId === id) {
                    editor.contentItem.contentY = editor.scrollPositions[id] || 0
                    if (editor.visible) editor.focusName()
                }
            })
        }
    }

    function loadResourcePreset() {
        systemResourceDefaults.checked = resources.presetIndex < 0
        resourcePreset.currentIndex = Math.max(0, resources.presetIndex)
    }

    function focusName() {
        function inView(control) {
            if (!control.visible || !control.enabled) return false
            const position = control.mapToItem(editor, 0, 0)
            return position.y >= 0 && position.y + control.height <= editor.height
        }
        function firstVisibleControl(item) {
            if (item.activeFocusOnTab && inView(item)) return item
            for (const child of item.children || []) {
                const control = firstVisibleControl(child)
                if (control) return control
            }
            return null
        }
        // A returned editor retains its scroll position; avoid focusing a name
        // field above the viewport and sending subsequent typing offscreen.
        const target = inView(setNameField) ? setNameField : firstVisibleControl(contentItem)
        if (target) target.forceActiveFocus()
        else editor.forceActiveFocus()
    }

    function addSource(url) {
        const path = LocalPaths.localPath(url)
        if (path.length > 0) {
            sourceModel.append({ path: path })
        }
    }

    function addSources(urls) {
        urls.forEach(function (url) { addSource(url) })
    }

    function addExclusion(url) {
        const path = LocalPaths.localPath(url)
        const exclusions = lines(exclusionsField.text)
        if (path.length > 0 && exclusions.indexOf(path) < 0) {
            exclusions.push(path)
            exclusionsField.text = exclusions.join("\n")
        }
    }

    function syncCurrentSet() {
        syncingCurrentSet = true
        try {
            const sources = []
            for (let index = 0; index < sourceModel.count; ++index) {
                sources.push(sourceModel.get(index).path)
            }
            const timeParts = scheduleTimeField.text.split(":")
            controller.applyCurrentDraft({
                name: setNameField.text,
                remoteRoot: remoteField.text,
                sources: sources,
                exclusions: lines(exclusionsField.text),
                scheduleFrequency: scheduleFrequency.currentText,
                scheduleHour: Number(timeParts[0]),
                scheduleMinute: Number(timeParts[1]),
                scheduleWeekday: scheduleWeekday.currentIndex + 1,
                scheduleDayOfMonth: scheduleDay.value,
                retention: retentionSpin.value,
                onlyOnAcPower: acPowerCheck.checked
            })
        } finally {
            syncingCurrentSet = false
        }
    }

    Component.onCompleted: loadCurrentSet()

    Connections {
        target: editor.controller
        function onCurrentSetChanged() {
            if (!editor.syncingCurrentSet) {
                editor.loadCurrentSet()
            }
        }
    }

    Connections {
        target: editor.resources
        function onPresetChanged() { editor.loadResourcePreset() }
    }

    FormCard {
        id: editorCard
        objectName: "editorCard"
        style: editor.style
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: editor.style.contentPadding
        anchors.rightMargin: editor.style.contentPadding

        Label {
            text: qsTr("Name")
            font.pixelSize: editor.style.sectionTitleSize
            font.weight: Font.Normal
            color: editor.style.accentColor
        }

        TextField {
            id: setNameField
            objectName: "setNameField"
            placeholderText: qsTr("Example: Documents")
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: editor.style.buttonSpacing

            Label {
                text: qsTr("Source files and folders")
                font.pixelSize: editor.style.sectionTitleSize
                font.weight: Font.Normal
                color: editor.style.accentColor
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.minimumWidth: 0
            }

            ActionButton {
                id: addSourceButton
                style: editor.style
                objectName: "addSourceButton"
                text: "+"
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Add a source file or folder")
                onClicked: sourceMenu.open()
            }
        }

        Label {
            text: qsTr("Choose files or folders to include in this backup.")
            font.pixelSize: editor.style.metadataTypeSize
            lineHeight: editor.style.bodyLeading
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        FileDialog {
            id: sourceFilesDialog
            title: qsTr("Select source files")
            fileMode: FileDialog.OpenFiles
            onAccepted: editor.addSources(selectedFiles)
        }

        FolderDialog {
            id: sourceFolderDialog
            title: qsTr("Select source folder")
            onAccepted: editor.addSource(selectedFolder)
        }

        Menu {
            id: sourceMenu
            objectName: "sourceMenu"
            parent: addSourceButton
            x: addSourceButton.width - width
            y: addSourceButton.height

            MenuItem {
                text: qsTr("Add files")
                onTriggered: sourceFilesDialog.open()
            }

            MenuItem {
                text: qsTr("Add folder")
                onTriggered: sourceFolderDialog.open()
            }
        }

        Label {
            text: qsTr("No source files or folders selected yet.")
            font.pixelSize: editor.style.metadataTypeSize
            visible: sourceModel.count === 0
            Layout.fillWidth: true
        }

        ListView {
            id: sourceList
            model: sourceModel
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(48, Math.min(180, contentHeight))
            clip: true

            delegate: RowLayout {
                required property int index
                required property string path
                width: sourceList.width
                spacing: editor.style.buttonSpacing

                TextField {
                    text: path
                    placeholderText: qsTr("Source path")
                    Layout.fillWidth: true
                    onTextChanged: {
                        if (index >= 0 && index < sourceModel.count && sourceModel.get(index).path !== text) {
                            sourceModel.setProperty(index, "path", text)
                        }
                    }
                }

                ActionButton {
                    style: editor.style
                    text: "-"
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Remove this source")
                    onClicked: sourceModel.remove(index)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: editor.style.buttonSpacing

            Label {
                text: qsTr("Exclusions (optional)")
                font.pixelSize: editor.style.sectionTitleSize
                font.weight: Font.Normal
                color: editor.style.accentColor
                Layout.fillWidth: true
            }

            ActionButton {
                id: addExclusionButton
                style: editor.style
                objectName: "addExclusionButton"
                text: "+"
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Exclude a file or folder")
                onClicked: exclusionMenu.open()
            }
        }

        Menu {
            id: exclusionMenu
            objectName: "exclusionMenu"
            parent: addExclusionButton
            x: addExclusionButton.width - width
            y: addExclusionButton.height

            MenuItem {
                text: qsTr("Exclude files")
                onTriggered: exclusionFilesDialog.open()
            }

            MenuItem {
                text: qsTr("Exclude folder")
                onTriggered: exclusionFolderDialog.open()
            }
        }

        FileDialog {
            id: exclusionFilesDialog
            title: qsTr("Select files to exclude")
            fileMode: FileDialog.OpenFiles
            onAccepted: selectedFiles.forEach(function (url) { editor.addExclusion(url) })
        }

        FolderDialog {
            id: exclusionFolderDialog
            title: qsTr("Select folder to exclude")
            onAccepted: editor.addExclusion(selectedFolder)
        }

        TextArea {
            id: exclusionsField
            placeholderText: qsTr("Full paths or folder names, one per line (e.g. node_modules)")
            wrapMode: TextArea.Wrap
            Layout.fillWidth: true
            Layout.preferredHeight: 72
        }

        Flow {
            width: parent.width
            spacing: 12

            Label {
                text: qsTr("Schedule")
                font.pixelSize: editor.style.bodyTypeSize
            }

            ComboBox {
                id: scheduleFrequency
                objectName: "scheduleFrequency"
                model: ["disabled", "daily", "weekly", "monthly"]
                width: 130
            }

            TextField {
                id: scheduleTimeField
                text: "02:00"
                placeholderText: qsTr("HH:MM")
                width: 90
                visible: scheduleFrequency.currentText !== "disabled"
            }
        }

        Flow {
            visible: scheduleFrequency.currentText === "weekly"
            width: parent.width
            spacing: 12

            Label {
                text: qsTr("Run every week on:")
                font.pixelSize: editor.style.bodyTypeSize
            }

            ComboBox {
                id: scheduleWeekday
                model: [qsTr("Monday"), qsTr("Tuesday"), qsTr("Wednesday"), qsTr("Thursday"), qsTr("Friday"), qsTr("Saturday"), qsTr("Sunday")]
                width: 120
            }
        }

        Flow {
            visible: scheduleFrequency.currentText === "monthly"
            width: parent.width
            spacing: 12

            Label {
                text: qsTr("Run every month on day:")
                font.pixelSize: editor.style.bodyTypeSize
            }

            SpinBox {
                id: scheduleDay
                from: 1
                to: 31
                value: 1
                editable: true
                width: 70
            }
        }

        Label {
            text: scheduleFrequency.currentText === "disabled"
                ? qsTr("Scheduling is disabled.")
                : scheduleFrequency.currentText === "daily"
                    ? qsTr("This backup runs every day at the selected time.")
                    : scheduleFrequency.currentText === "weekly"
                        ? qsTr("This backup runs every week at the selected time and weekday.")
                        : qsTr("This backup runs every month at the selected time. Days 29-31 use the last day when a month is shorter.")
            wrapMode: Text.WordWrap
            font.pixelSize: editor.style.metadataTypeSize
            lineHeight: editor.style.bodyLeading
            lineHeightMode: Text.ProportionalHeight
            Layout.fillWidth: true
        }

        Label {
            text: qsTr("Next run: %1").arg(editor.controller.currentNextRun)
            font.pixelSize: editor.style.metadataTypeSize
        }

        ActionButton {
            style: editor.style
            objectName: "advancedSettingsButton"
            text: qsTr("Advanced settings")
            checkable: true
            checked: editor.showAdvanced
            Layout.alignment: Qt.AlignRight
            onClicked: editor.showAdvanced = checked
        }

        GroupBox {
            objectName: "advancedSettingsPanel"
            visible: editor.showAdvanced
            title: qsTr("Advanced settings")
            Layout.fillWidth: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 12

                Label {
                    text: qsTr("Remote Proton Drive folder")
                    font.pixelSize: editor.style.sectionTitleSize
                    font.weight: Font.Normal
                    color: editor.style.accentColor
                }

                Label {
                    text: qsTr("Copies are saved in Proton Drive under this folder, grouped by computer and backup name.")
                    font.pixelSize: editor.style.bodyTypeSize
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                TextField {
                    id: remoteField
                    placeholderText: qsTr("Example: /my-files/backups")
                    Layout.fillWidth: true
                }

                RowLayout {
                    Label {
                        text: qsTr("Successful copies to keep")
                        font.pixelSize: editor.style.bodyTypeSize
                    }

                    SpinBox {
                        id: retentionSpin
                        from: 1
                        to: 100
                        value: 3
                        editable: true
                    }
                }

                CheckBox {
                    id: acPowerCheck
                    text: qsTr("Only back up on AC power")
                }

                Label {
                    text: qsTr("Resource usage (all backups)")
                    font.pixelSize: editor.style.sectionTitleSize
                    font.weight: Font.Normal
                    color: editor.style.accentColor
                }

                CheckBox {
                    id: systemResourceDefaults
                    objectName: "useSystemResourceDefaults"
                    text: qsTr("Use system defaults")
                    enabled: !editor.resources.busy
                }

                ComboBox {
                    id: resourcePreset
                    objectName: "resourceUsagePreset"
                    model: editor.resources.names
                    enabled: !editor.resources.busy && !systemResourceDefaults.checked
                    Layout.fillWidth: true
                    Accessible.name: qsTr("Backup resource usage")
                }

                Label {
                    objectName: "resourceUsageDescription"
                    text: systemResourceDefaults.checked
                        ? qsTr("No CPU cap · Normal CPU priority · Normal I/O scheduling")
                        : editor.resources.descriptions[resourcePreset.currentIndex] || ""
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                Label {
                    text: qsTr("Applies to all manual and scheduled backups after Save, when the next worker starts. 100% allows one full CPU core; 200% allows two. A lower nice value gives the worker higher CPU priority.")
                    font.pixelSize: editor.style.metadataTypeSize
                    color: editor.style.mutedColor
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }

        ColumnLayout {
            objectName: "backupPreview"
            visible: editor.controller.previewAvailable
            Layout.fillWidth: true
            spacing: 12

            Label {
                text: qsTr("Backup preview")
                font.pixelSize: editor.style.sectionTitleSize
                font.weight: Font.Normal
                color: editor.style.accentColor
            }

            Label {
                text: qsTr("Review this selection before backing up. Preview does not save settings, run a backup, or enable scheduling. Only included files will be attempted; skipped and missing paths will not be backed up.")
                font.pixelSize: editor.style.metadataTypeSize
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            PreviewGroup {
                style: editor.style
                key: "included"
                heading: qsTr("Included")
                emptyText: qsTr("No files will be backed up from this selection.")
                paths: editor.controller.previewIncluded
            }

            PreviewGroup {
                style: editor.style
                key: "excluded"
                heading: qsTr("Excluded")
                emptyText: qsTr("No paths matched the exclusions.")
                paths: editor.controller.previewExcluded
            }

            PreviewGroup {
                style: editor.style
                key: "skipped"
                heading: qsTr("Skipped")
                emptyText: qsTr("No unreadable or unsupported paths were skipped.")
                paths: editor.controller.previewSkipped
            }

            PreviewGroup {
                style: editor.style
                key: "missing"
                heading: qsTr("Missing")
                emptyText: qsTr("No source paths are missing.")
                paths: editor.controller.previewMissing
            }
        }

        RowLayout {
            visible: editor.controller.previewBusy
            Layout.fillWidth: true
            BusyIndicator {
                objectName: "previewLoadingIndicator"
                running: editor.controller.previewBusy
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
            }
            Label {
                objectName: "previewLoadingMessage"
                text: editor.controller.previewAvailable
                    ? qsTr("Scanning sources… Showing the previous preview while refreshing.")
                    : qsTr("Scanning sources… You can continue editing or return to the dashboard.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        Label {
            text: qsTr("Retention cleanup is waiting for confirmation. Proposed deletions:")
            font.pixelSize: editor.style.bodyTypeSize
            lineHeight: editor.style.bodyLeading
            lineHeightMode: Text.ProportionalHeight
            visible: editor.controller.cleanupConfirmationRequired
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        ListView {
            model: editor.controller.cleanupTargets
            visible: editor.controller.cleanupConfirmationRequired
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(100, contentHeight)
            clip: true
            delegate: Label {
                required property string modelData
                text: modelData
                font.pixelSize: editor.style.metadataTypeSize
                width: parent ? parent.width : 0
                elide: Text.ElideMiddle
            }
        }

        ActionButton {
            style: editor.style
            text: qsTr("Confirm proposed cleanup")
            visible: editor.controller.cleanupConfirmationRequired
            Layout.alignment: Qt.AlignRight
            onClicked: editor.controller.confirmCleanup()
        }

        RowLayout {
            visible: editor.backupRunning
            Layout.fillWidth: true

            BusyIndicator {
                running: editor.backupRunning
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
            }

            Label {
                text: qsTr("Backup in progress… %1").arg(
                    editor.controller.remainingTimes[editor.controller.currentId]
                        || qsTr("Estimating time remaining…"))
                font.pixelSize: editor.style.metadataTypeSize
                color: editor.style.accentColor
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        Label {
            objectName: "editorTransferProgress"
            text: (editor.controller.transferProgress[editor.controller.currentId] || {}).text || ""
            visible: editor.backupRunning && text.length > 0
            textFormat: Text.PlainText
            font.pixelSize: editor.style.metadataTypeSize
            wrapMode: Text.WrapAnywhere
            Layout.fillWidth: true
        }

        RowLayout {
            objectName: "editorActionsRow"
            Layout.fillWidth: true
            spacing: editor.style.buttonSpacing

            Item { Layout.fillWidth: true }

            ActionButton {
                style: editor.style
                objectName: "saveBackupSetButton"
                text: qsTr("Save")
                enabled: !editor.resources.busy
                onClicked: {
                    editor.syncCurrentSet()
                    if (editor.controller.save()) {
                        editor.resources.save(systemResourceDefaults.checked ? -1 : resourcePreset.currentIndex)
                        editor.saved()
                    }
                }
            }

            ActionButton {
                style: editor.style
                objectName: "previewBackupSetButton"
                text: qsTr("Preview")
                enabled: !editor.controller.previewBusy
                onClicked: {
                    editor.syncCurrentSet()
                    editor.controller.preview()
                }
            }

            ActionButton {
                style: editor.style
                objectName: "closeEditorButton"
                text: qsTr("Cancel")
                Accessible.name: qsTr("Cancel backup settings")
                onClicked: editor.closeRequested()
            }
        }
    }
}

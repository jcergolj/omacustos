import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtTest
import "../qml" as OmaCustos

TestCase {
    id: testCase
    name: "Dashboard"
    when: windowShown
    property var app

    Component { id: windowComponent; OmaCustos.Main {} }

    function init() {
        backupScheduler.busy = false
        backupScheduler.ready = true
        backupScheduler.hasSchedules = true
        backupScheduler.status = "Scheduling active"
        backupScheduler.error = ""
        backupScheduler.enableCount = 0
        backupScheduler.refreshCount = 0
        resourceUsage.presetIndex = -1
        resourceUsage.busy = false
        resourceUsage.savedIndex = -2
        protonAuth.authenticated = true
        protonAuth.checked = true
        protonAuth.checking = false
        protonAuth.cliAvailable = true
        protonAuth.error = ""
        protonAuth.signInCount = 0
        protonAuth.refreshCount = 0
        themeColors.colors = {
            background: "#ffffff", foreground: "#19232e", muted: "#586575",
            surface: "#f0f3f6", border: "#dce1e7", accent: "#245bcb",
            highlight: "#245bcb", highlightedText: "#ffffff", brightText: "#ffffff"
        }
        backupSetController.setNames = ["Documents", "Photos"]
        backupSetController.setIds = ["documents-id", "photos-id"]
        backupSetController.currentIndex = 0
        backupSetController.currentName = Qt.binding(function () { return backupSetController.setNames[backupSetController.currentIndex] || "" })
        backupSetController.currentRemoteRoot = Qt.binding(function () { return "/backups/" + backupSetController.currentId })
        backupSetController.currentSources = Qt.binding(function () { return ["/safe/" + backupSetController.currentId] })
        backupSetController.previewBusy = false
        backupSetController.dashboardRefreshError = ""
        backupSetController.runningSetIds = []
        backupSetController.remainingTimes = {}
        backupSetController.transferProgress = {}
        backupSetController.runDetails = {}
        backupSetController.removedIndex = -1
        backupSetController.addedCount = 0
        backupSetController.refreshCount = 0
        backupSetController.importedPath = ""
        backupSetController.importedMerge = false
        backupSetController.exportedPath = ""
        backupSetController.templatePath = ""
        backupSetController.previewAvailable = false
        backupSetController.previewCount = 0
        backupSetController.saveCount = 0
        backupSetController.previewIncluded = []
        backupSetController.previewExcluded = []
        backupSetController.previewSkipped = []
        backupSetController.previewMissing = []
        backupSetController.importSucceeds = true
        backupSetController.recentBackups = ["Photos\nNo backup run yet", "Documents\nsucceeded"]
        backupSetController.recentBackupSetIds = ["photos-id", "documents-id"]
        backupSetController.recentBackupTimestamps = ["", "01/10/2026 10:00:00"]
        backupLauncher.launchedId = ""
        restoreController.discoveredRoot = ""
        restoreController.discoveredSetId = ""
        restoreController.busy = false
        restoreController.restoring = false
        restoreController.restoreProgress = ""
        restoreController.browseError = ""
        restoreController.loadingMessage = ""
        restoreController.showingCachedData = false
        restoreController.verified = false
        restoreController.currentCopyIndex = -1
        restoreController.selectedCopy = -1
        restoreController.copySearch = ""
        restoreController.entries = []
        restoreController.copies = []
        restoreController.unavailableEntries = []
        restoreController.restoredIndexes = []
        restoreController.restoreDestination = ""
        restoreController.restoreCount = 0
        restoreController.restoreSucceeds = true
        protonFolderBrowser.requestedPath = ""
        protonFolderBrowser.busy = false
        recentBackupCopies.busy = false
        recentBackupCopies.deletingSetId = ""
        recentBackupCopies.openedId = ""
        recentBackupCopies.deleteRequestedId = ""
        recentBackupCopies.deleteConfirmed = false
        recentBackupCopies.deleteCancelled = false
        app = createTemporaryObject(windowComponent, testCase)
        verify(app !== null)
        app.requestActivate()
        waitForRendering(app.contentItem)
    }

    function control(name) {
        let item = null
        tryVerify(function () {
            item = findChild(app, name)
            if (item === null) {
                for (const listName of ["dashboardSetsList", "recentBackupsList", "restoreFilesList"]) {
                    const list = findChild(app, listName)
                    for (let index = 0; list && index < list.count; ++index) {
                        const row = list.itemAtIndex(index)
                        if (row) {
                            if (row.objectName === name) {
                                item = row
                                return true
                            }
                            item = findChild(row, name)
                            if (item) {
                                return true
                            }
                        }
                    }
                }
            }
            return item !== null
        }, 1000, "Missing UI control: " + name)
        return item
    }

    function visibleTexts(item) {
        if (!item.visible) {
            return []
        }
        let texts = typeof item.text === "string" ? [item.text] : []
        for (const child of item.children || []) {
            texts = texts.concat(visibleTexts(child))
        }
        return texts
    }

    function test_setsAreLeftOfRecentBackups() {
        compare(control("backupSetsTitle").text, "Backup sets")
        compare(control("recentBackupsTitle").text, "Recent backups")
        compare(control("newBackupSetButton").text, "+")
        compare(control("newBackupSetButton").Accessible.name, "New backup set")
        const sets = control("dashboardSetsList")
        const recent = control("recentBackupsList")
        const left = sets.mapToItem(app.contentItem, 0, 0)
        const right = recent.mapToItem(app.contentItem, 0, 0)
        verify(left.x + sets.width < right.x, "Backup sets must be in the left column")
        verify(Math.abs(left.y - right.y) < 2, "The two lists must align")
        verify(Math.abs(recent.width - 2 * sets.width) < 2, "Recent backups must receive two-thirds of the column space")
        verify(visibleTexts(app.contentItem).indexOf("Backups") < 0)
    }

    function openMenu(index) {
        const button = control("setActions-" + index)
        mouseClick(button)
        const menu = control("setMenu-" + index)
        tryCompare(menu, "opened", true)
        return menu
    }

    function test_editLoadsTheMenuTarget() {
        const menu = openMenu(1)
        compare(menu.itemAt(0).text, "Edit")
        mouseClick(menu.itemAt(0))
        tryCompare(app, "showEditor", true)
        compare(backupSetController.currentIndex, 1)
        tryCompare(control("setNameField"), "text", "Photos")
        compare(backupLauncher.launchedId, "")
    }

    function test_backupLaunchesTheMenuTargetWithoutChangingSelection() {
        const menu = openMenu(1)
        compare(menu.itemAt(1).text, "Back up now")
        mouseClick(menu.itemAt(1))
        compare(backupLauncher.launchedId, "photos-id")
        compare(backupSetController.currentIndex, 0)
        compare(app.showEditor, false)
        compare(control("recentSummary-0").text, "Photos · No backup run yet")
        compare(control("restore-0").enabled, false)
    }

    function test_deleteRequiresConfirmationAndCancelDoesNotRemove() {
        const menu = openMenu(1)
        compare(menu.itemAt(3).text, "Delete")
        mouseClick(menu.itemAt(3))
        const dialog = control("removeSetDialog")
        tryCompare(dialog, "opened", true)
        compare(dialog.setName, "Photos")
        compare(dialog.title, "Delete backup set")
        verify(dialog.contentItem.text.indexOf("configuration and schedule") >= 0)
        verify(dialog.contentItem.text.indexOf("Proton Drive will remain") >= 0)
        compare(backupSetController.removedIndex, -1)
        mouseClick(dialog.standardButton(Dialog.Cancel))
        tryCompare(dialog, "visible", false)
        compare(backupSetController.removedIndex, -1)

        mouseClick(openMenu(1).itemAt(3))
        tryCompare(dialog, "opened", true)
        mouseClick(dialog.standardButton(Dialog.Ok))
        compare(backupSetController.removedIndex, 1)
    }

    function test_runningSetCannotLaunchAgain() {
        backupSetController.runningSetIds = ["photos-id"]
        const indicator = control("setBusy-1")
        tryCompare(indicator, "visible", true)
        compare(indicator.running, true)
        const menu = openMenu(1)
        compare(menu.itemAt(1).enabled, false)
        compare(menu.itemAt(0).enabled, true)
        mouseClick(menu.itemAt(1))
        compare(backupLauncher.launchedId, "")
        menu.close()
        tryCompare(menu, "visible", false)
        compare(openMenu(0).itemAt(1).enabled, true)
    }

    function test_runningBackupShowsItsRemainingTimeAndHidesItWhenFinished() {
        backupSetController.runningSetIds = ["photos-id"]
        const remaining = control("setRemainingTime-1")
        tryCompare(remaining, "visible", true)
        compare(remaining.text, "Estimating time remaining…")
        backupSetController.remainingTimes = { "photos-id": "Est. remaining: 04:32" }
        tryCompare(remaining, "text", "Est. remaining: 04:32")
        // The running set's estimate is independent of the selected editor set.
        compare(backupSetController.currentIndex, 0)
        backupSetController.remainingTimes = { "photos-id": "Finalizing backup…" }
        tryCompare(remaining, "text", "Finalizing backup…")
        backupSetController.runningSetIds = []
        tryCompare(remaining, "visible", false)
    }

    function test_transferProgressShowsCurrentFileAndKeepsFailuresSeparate() {
        backupSetController.runningSetIds = ["photos-id"]
        backupSetController.transferProgress = {
            "photos-id": {
                text: "2 of 4 files processed · 1 verified · 1 items failed\nUploading: /safe/large file (2 GiB)",
                fraction: 0.5, indeterminate: false
            }
        }
        const progress = control("setTransferProgress-1")
        tryCompare(progress, "visible", true)
        verify(progress.text.indexOf("1 verified · 1 items failed") >= 0)
        verify(progress.text.indexOf("Uploading: /safe/large file") >= 0)
        const bar = control("setProgressBar-1")
        compare(bar.value, 0.5)
        compare(bar.indeterminate, false)
        backupSetController.transferProgress = {
            "photos-id": { text: "4 of 4 files processed · 3 verified · 1 items failed", fraction: 1, indeterminate: true }
        }
        tryCompare(bar, "indeterminate", true)
        backupSetController.runningSetIds = []
        tryCompare(progress, "visible", false)
        compare(bar.visible, false)
    }

    function test_incompleteResultDetailsShowPathsAndReasonsAndAllowRestore() {
        backupSetController.recentBackups = ["Photos\nNo backup run yet", "Documents\nIncomplete"]
        backupSetController.runDetails = {
            "documents-id": {
                status: "Incomplete", summary: "97 files backed up · 3 items failed",
                copyPath: "/backups/documents-id/copy", error: "Backup incomplete", nextAttempt: "02/10/2026 12:00:00",
                issues: [
                    { path: "/safe/a", phase: "Uploading", reason: "Connection interrupted" },
                    { path: "/safe/b", phase: "Uploading", reason: "Connection interrupted" },
                    { path: "/safe/c", phase: "Reading", reason: "Permission denied" }
                ]
            }
        }
        tryCompare(control("recentResultSummary-1"), "text", "97 files backed up · 3 items failed")
        verify(control("recentResultSummary-1").visible)
        verify(control("restore-1").enabled)
        const menu = openRecentMenu(1)
        compare(menu.itemAt(1).text, "View details")
        verify(menu.itemAt(1).enabled)
        mouseClick(menu.itemAt(1))
        const dialog = control("backupDetailsDialog")
        tryCompare(dialog, "opened", true)
        compare(dialog.setId, "documents-id")
        compare(control("backupDetailsStatus").text, "Incomplete")
        compare(control("backupDetailsSummary").text, "97 files backed up · 3 items failed")
        const issues = control("backupIssues")
        tryCompare(issues, "count", 3)
        const lastIssue = issues.itemAt(2)
        verify(lastIssue !== null)
        compare(findChild(lastIssue, "backupIssuePath-2").text, "/safe/c")
        compare(findChild(lastIssue, "backupIssueReason-2").text, "Reading: Permission denied")
        // An open dialog must stay tied to its backup identity when history reorders.
        backupSetController.recentBackupSetIds = ["documents-id", "photos-id"]
        compare(dialog.setId, "documents-id")
        mouseClick(dialog.standardButton(Dialog.Close))
        tryCompare(dialog, "visible", false)
    }

    function test_setDeletionUsesIdentityAfterListOrderChanges() {
        mouseClick(openMenu(1).itemAt(3))
        const dialog = control("removeSetDialog")
        tryCompare(dialog, "opened", true)
        backupSetController.setNames = ["Photos", "Documents"]
        backupSetController.setIds = ["photos-id", "documents-id"]
        mouseClick(dialog.standardButton(Dialog.Ok))
        compare(backupSetController.removedIndex, 0)
    }

    function test_escapeCancelsDeleteConfirmation() {
        mouseClick(openMenu(1).itemAt(3))
        const dialog = control("removeSetDialog")
        tryCompare(dialog, "opened", true)
        keyClick(Qt.Key_Escape)
        tryCompare(dialog, "visible", false)
        compare(backupSetController.removedIndex, -1)
    }

    function test_restoreUsesHistorySetIdAndRequiresActivity() {
        compare(control("restore-0").enabled, false)
        mouseClick(control("restore-0"))
        compare(restoreController.discoveredRoot, "")
        compare(app.showRestore, false)
        backupSetController.currentIndex = 1
        mouseClick(control("restore-1"))
        compare(backupSetController.currentIndex, 1)
        compare(restoreController.discoveredRoot, "/backups/documents-id")
        compare(restoreController.discoveredSetId, "documents-id")
        compare(app.showRestore, true)
        compare(control("dashboardScrollView").visible, false)
        compare(control("editorScrollView").visible, false)
        compare(control("restoreScrollView").visible, true)
        compare(control("closeRestoreButton").text, "×")
        compare(control("closeRestoreButton").Accessible.name, "Close restore")
        compare(control("recentSummary-1").text, "Documents · succeeded · 01/10/2026 10:00:00")
    }

    function test_unknownHistorySetCannotRestore() {
        backupSetController.recentBackupSetIds = ["photos-id", "removed-id"]
        compare(control("restore-1").enabled, false)
        mouseClick(control("restore-1"))
        compare(restoreController.discoveredRoot, "")
    }

    function test_restoreStaysOpenWhileLoadingAndRequiresExplicitCopySelection() {
        mouseClick(control("restore-1"))
        restoreController.busy = true
        restoreController.loadingMessage = "Loading backup copies…"
        compare(app.showRestore, true)
        compare(control("restorePanel").visible, true)
        verify(control("restoreLoadingIndicator").running)
        compare(control("restoreLoadingMessage").text, "Loading backup copies…")
        compare(control("restoreCopySelector").enabled, false)
        compare(control("restoreCopySearch").enabled, true)
        compare(control("restore-1").enabled, true)
        compare(control("startRestoreButton").enabled, false)

        restoreController.copies = ["Computer / Documents / copy-id"]
        restoreController.busy = false
        const selector = control("restoreCopySelector")
        compare(selector.enabled, true)
        compare(selector.currentIndex, -1)
        compare(restoreController.selectedCopy, -1)
        selector.currentIndex = 0
        selector.activated(0)
        compare(restoreController.selectedCopy, 0)
        restoreController.busy = true
        restoreController.loadingMessage = "Loading and verifying files…"
        compare(control("restoreLoadingMessage").text, "Loading and verifying files…")
        compare(selector.enabled, true)
    }

    function test_restoreNavigationAndFocusRemainAvailableDuringTransfer() {
        mouseClick(control("restore-1"))
        restoreController.restoring = true
        restoreController.busy = true
        restoreController.restoreProgress = "0 of 2 files restored"
        restoreController.restoreBackupId = "documents-id"
        restoreController.restoreBackupFolder = "/backups/documents-id"
        verify(control("restore-1").enabled)
        mouseClick(control("closeRestoreButton"))
        compare(app.showRestore, false)
        compare(control("dashboardScrollView").visible, true)
        compare(control("restoreScrollView").visible, false)
        verify(control("activeRestoreProgress").visible)
        mouseClick(control("returnToRestoreButton"))
        compare(app.showRestore, true)
        tryCompare(control("restoreCopySearch"), "activeFocus", true)
        compare(restoreController.restoring, true)
        compare(control("startRestoreButton").enabled, false)
    }

    function test_returningToRestorePreservesDestinationTicksAndScroll() {
        showRestoreFiles()
        mouseClick(control("restoreFile-0"))
        const destination = control("restoreDestinationField")
        destination.text = "/safe/return-here"
        const list = control("restoreFilesList")
        mouseClick(control("closeRestoreButton"))
        mouseClick(control("restore-1"))
        compare(destination.text, "/safe/return-here")
        compare(app.selectedRestoreIndexes, [0])
        compare(control("restoreFile-0").checked, true)
        compare(list.contentY, 0)
    }

    function test_largeRestoreKeepsOffscreenSelectionsAndScrollAcrossRefreshAndReturn() {
        mouseClick(control("restore-1"))
        const paths = []
        const indexes = []
        const copies = []
        for (let index = 0; index < 10000; ++index) {
            paths.push("/safe/documents/file-" + index + ".txt")
            indexes.push(index)
        }
        for (let copy = 0; copy < 100; ++copy) copies.push("Documents / copy-" + copy)
        restoreController.copies = copies
        restoreController.currentCopyIndex = 0
        restoreController.entries = paths
        restoreController.verified = true
        app.selectedRestorePaths = paths.slice()
        app.selectedRestoreIndexes = indexes
        const list = control("restoreFilesList")
        tryCompare(list, "count", 10000)
        list.positionViewAtIndex(5000, ListView.Center)
        waitForRendering(app.contentItem)
        verify(control("restoreFile-5000").checked)
        const selectedIndexes = app.selectedRestoreIndexes
        const selectedPaths = app.selectedRestorePaths
        const lookup = control("restorePanel").selectionLookup
        // Exercise the actual checkbox handler, including a large offscreen
        // selection, without depending on instantiated delegates for its state.
        control("restoreFile-5000").toggle()
        control("restoreFile-5000").toggled()
        compare(app.selectedRestoreIndexes.length, 9999)
        verify(app.selectedRestoreIndexes.indexOf(5000) < 0)
        verify(app.selectedRestoreIndexes === selectedIndexes)
        verify(app.selectedRestorePaths === selectedPaths)
        verify(control("restorePanel").selectionLookup === lookup)
        compare(control("restoreSelectionCount").text, "Selected files: 9999")
        // Deselecting swaps in the last selection. Toggling that moved item
        // must use its updated position and keep path/index arrays aligned.
        list.positionViewAtIndex(9999, ListView.Center)
        waitForRendering(app.contentItem)
        control("restoreFile-9999").toggle()
        control("restoreFile-9999").toggled()
        compare(app.selectedRestoreIndexes.length, 9998)
        verify(app.selectedRestoreIndexes.indexOf(9999) < 0)
        verify(app.selectedRestorePaths.indexOf(paths[9999]) < 0)
        control("restoreFile-9999").toggle()
        control("restoreFile-9999").toggled()
        compare(app.selectedRestoreIndexes.length, 9999)
        compare(app.selectedRestorePaths[app.selectedRestoreIndexes.indexOf(9999)], paths[9999])
        list.positionViewAtIndex(5000, ListView.Center)
        waitForRendering(app.contentItem)
        const scrollY = list.contentY
        mouseClick(control("closeRestoreButton"))
        mouseClick(control("restore-1"))
        tryCompare(list, "contentY", scrollY)
        verify(!control("restoreFile-5000").checked)
        restoreController.entries = paths.slice().reverse()
        compare(app.selectedRestoreIndexes.length, 9999)
        verify(app.selectedRestoreIndexes.indexOf(4999) < 0)
        verify(app.selectedRestoreIndexes.indexOf(9999) >= 0)
        restoreController.entries = paths.slice(1)
        compare(app.selectedRestoreIndexes.length, 9998)
        verify(app.selectedRestorePaths.indexOf(paths[0]) < 0)
    }

    function test_previewLoadingAllowsTypingAndNavigationAndShowsLastGoodData() {
        mouseClick(control("newBackupSetButton"))
        backupSetController.previewAvailable = true
        backupSetController.previewIncluded = ["/safe/previous.txt"]
        backupSetController.previewBusy = true
        verify(control("previewLoadingIndicator").running)
        verify(control("previewLoadingMessage").text.indexOf("previous preview") >= 0)
        verify(!control("previewBackupSetButton").enabled)
        const name = control("setNameField")
        tryCompare(name, "text", "New set")
        name.forceActiveFocus()
        name.cursorPosition = name.text.length
        keyClick(Qt.Key_X)
        verify(name.text.endsWith("x"))
        mouseClick(control("closeEditorButton"))
        compare(app.showEditor, false)
        compare(backupSetController.previewBusy, true)
    }

    function test_editorReturnPreservesScrollForEachBackup() {
        app.editSet(0)
        const editor = control("editorScrollView")
        tryCompare(control("setNameField"), "text", "Documents")
        app.showAdvanced = true
        waitForRendering(app.contentItem)
        editor.contentItem.contentY = 300
        const scrollY = editor.contentItem.contentY
        app.editSet(1)
        tryCompare(control("setNameField"), "text", "Photos")
        app.editSet(0)
        tryCompare(control("setNameField"), "text", "Documents")
        tryCompare(editor.contentItem, "contentY", scrollY)
        compare(app.showAdvanced, true)
        tryVerify(function () {
            const focused = app.activeFocusItem
            if (!focused) return false
            const position = focused.mapToItem(editor, 0, 0)
            return position.y >= 0 && position.y + focused.height <= editor.height
        }, 1000, "Returning to a scrolled editor should focus an in-view control")
    }

    function test_oldTransferCompletionDoesNotCloseAnotherContext() {
        showRestoreFiles()
        const oldFolder = restoreController.backupFolder
        const oldId = restoreController.backupId
        const oldCopy = restoreController.currentCopyPath
        backupSetController.recentBackupTimestamps = ["01/10/2026 10:00:00", "01/10/2026 10:00:00"]
        app.restoreRecentBackup(0)
        control("restoreDestinationField").text = "/safe/photos"
        restoreController.restoreCompletedForContext(oldFolder, oldId, oldCopy)
        compare(app.showRestore, true)
        compare(control("restoreDestinationField").text, "/safe/photos")
    }

    function showRestoreFiles() {
        mouseClick(control("restore-1"))
        restoreController.copies = ["Computer / Documents / copy-id"]
        restoreController.entries = ["/safe/documents/notes.txt", "/safe/documents/photo.jpg"]
        restoreController.verified = true
        waitForRendering(app.contentItem)
    }

    function test_restoreRequiresTickedFilesAndDestinationAndHasOneAction() {
        showRestoreFiles()
        const start = control("startRestoreButton")
        const destination = control("restoreDestinationField")
        compare(start.text, "Start restore")
        compare(start.font.weight, control("backupSetsMenuButton").font.weight)
        compare(start.font.pixelSize, control("backupSetsMenuButton").font.pixelSize)
        compare(control("chooseRestoreDestinationButton").font.weight, control("backupSetsMenuButton").font.weight)
        compare(start.enabled, false)
        compare(destination.text, "")
        verify(control("restoreInstructions").text.indexOf("tick the files") >= 0)
        mouseClick(control("restoreFile-0"))
        compare(app.selectedRestoreIndexes, [0])
        compare(app.selectedRestorePaths, ["/safe/documents/notes.txt"])
        compare(control("restoreSelectionCount").text, "Selected files: 1")
        compare(start.enabled, false)
        destination.text = "   "
        compare(start.enabled, false)
        destination.text = " /safe/chosen restore "
        compare(start.enabled, true)
        const texts = visibleTexts(control("restorePanel"))
        verify(texts.indexOf("Restore selected") < 0)
        verify(texts.indexOf("Restore folder") < 0)
        const startPosition = start.mapToItem(control("restorePanel"), 0, 0)
        const destinationPosition = destination.mapToItem(control("restorePanel"), 0, destination.height)
        verify(startPosition.y > destinationPosition.y)
        const scroll = control("restoreScrollView").contentItem
        scroll.contentY = scroll.contentHeight - scroll.height
        waitForRendering(app.contentItem)
        mouseClick(start)
        compare(restoreController.restoreCount, 1)
        compare(restoreController.restoredIndexes, [0])
        compare(restoreController.restoreDestination, "/safe/chosen restore")
        compare(app.showRestore, false)
        compare(app.selectedRestoreIndexes, [])
        compare(destination.text, "")
    }

    function test_failedRestoreKeepsThePanelAndSelectionOpenForRetry() {
        showRestoreFiles()
        mouseClick(control("restoreFile-0"))
        restoreController.restoreSucceeds = false
        const destination = control("restoreDestinationField")
        destination.text = "/safe/restore"
        const scroll = control("restoreScrollView").contentItem
        scroll.contentY = scroll.contentHeight - scroll.height
        waitForRendering(app.contentItem)
        mouseClick(control("startRestoreButton"))
        compare(app.showRestore, true)
        compare(app.selectedRestoreIndexes, [0])
        compare(destination.text, "/safe/restore")
        compare(control("notificationMessageLabel").text, "Restore failed")
    }

    function test_refreshingRestoreFilesPreservesValidTicksAndSelection() {
        showRestoreFiles()
        mouseClick(control("restoreFile-0"))
        compare(control("restoreFile-0").checked, true)
        restoreController.entriesChanged()
        compare(app.selectedRestoreIndexes, [0])
        compare(control("restoreFile-0").checked, true)
        compare(control("startRestoreButton").enabled, false)
    }

    function test_cachedRestoreFilesRequireVerificationAndDisableStartDuringTransfer() {
        showRestoreFiles()
        mouseClick(control("restoreFile-0"))
        control("restoreDestinationField").text = "/safe/restore"
        const start = control("startRestoreButton")
        compare(start.enabled, true)

        restoreController.verified = false
        restoreController.showingCachedData = true
        restoreController.busy = true
        const cachedMessage = control("restoreCachedDataMessage")
        compare(cachedMessage.visible, true)
        verify(cachedMessage.text.indexOf("verification finishes") >= 0)
        compare(start.enabled, false)
        compare(app.selectedRestoreIndexes, [0])

        restoreController.busy = false
        compare(cachedMessage.visible, true)
        verify(cachedMessage.text.indexOf("verify it again") >= 0)
        compare(start.enabled, false)
        restoreController.verified = true
        restoreController.showingCachedData = false
        compare(cachedMessage.visible, false)
        compare(start.enabled, true)

        restoreController.busy = true
        compare(start.enabled, false)
        control("restoreDestinationField").text = "/safe/retry"
        restoreController.busy = false
        restoreController.failed("Restore failed")
        compare(app.showRestore, true)
        compare(app.selectedRestoreIndexes, [0])
        compare(start.enabled, true)
    }

    function test_restoreDestinationPickerUsesChosenFolder() {
        showRestoreFiles()
        const dialog = control("restoreDestinationDialog")
        dialog.currentFolder = dashboardRestoreFolderUrl
        dialog.open()
        tryCompare(dialog, "visible", true)
        dialog.selectedFolder = dashboardRestoreFolderUrl
        dialog.accept()
        compare(control("restoreDestinationField").text, dashboardRestoreFolderPath)
        compare(control("startRestoreButton").enabled, false)
        mouseClick(control("restoreFile-1"))
        compare(app.selectedRestoreIndexes, [1])
        compare(control("startRestoreButton").enabled, true)
    }

    function test_folderLinkUsesHistorySetAndRequiresActivity() {
        compare(control("openFolder-0").enabled, false)
        mouseClick(control("openFolder-0"))
        compare(recentBackupCopies.openedId, "")
        backupSetController.currentIndex = 1
        const link = control("openFolder-1")
        compare(link.Accessible.name, "Open Documents in Proton Drive")
        mouseClick(link)
        compare(recentBackupCopies.openedId, "documents-id")
        recentBackupCopies.folderResolved("/backups/documents-id/copy-id")
        compare(protonFolderBrowser.requestedPath, "/backups/documents-id/copy-id")
        compare(backupSetController.currentIndex, 1)
        compare(app.showRestore, false)
        protonFolderBrowser.busy = true
        compare(link.enabled, false)
    }

    function test_unknownHistorySetCannotOpenFolder() {
        backupSetController.recentBackupSetIds = ["photos-id", "removed-id"]
        compare(control("openFolder-1").enabled, false)
        mouseClick(control("openFolder-1"))
        compare(recentBackupCopies.openedId, "")
    }

    function test_notificationsAppearTopRight_data() {
        return [
            { tag: "set status", source: "set", failure: false },
            { tag: "set failure", source: "set", failure: true },
            { tag: "launcher failure", source: "launcher", failure: true },
            { tag: "folder link failure", source: "folder", failure: true },
            { tag: "copy delete status", source: "copies", failure: false },
            { tag: "copy delete failure", source: "copies", failure: true },
            { tag: "restore status", source: "restore", failure: false },
            { tag: "restore failure", source: "restore", failure: true }
        ]
    }

    function test_notificationsAppearTopRight(data) {
        const controller = data.source === "set" ? backupSetController
            : data.source === "launcher" ? backupLauncher
            : data.source === "folder" ? protonFolderBrowser
            : data.source === "copies" ? recentBackupCopies : restoreController
        const message = data.failure ? "Permission denied" : "Preview ready"
        if (data.failure) {
            controller.failed(message)
        } else {
            controller.statusChanged(message)
        }
        const toast = control("notificationToast")
        const status = control("notificationMessageLabel")
        compare(status.text, message)
        tryCompare(toast, "opened", true)
        compare(toast.x + toast.width, app.width - app.contentPadding)
        compare(toast.y, app.contentPadding)
        compare(findChild(app, "dashboardStatusLabel"), null)
        compare(app.showRestore, false)
        if (dashboardScreenshotPath.length > 0 && data.source === "set" && !data.failure) {
            grabImage(app.contentItem).save(dashboardScreenshotPath + ".notification.png")
        }
    }

    function test_notificationTimeoutRestartsForNewMessage() {
        backupSetController.statusChanged("First message")
        const toast = control("notificationToast")
        tryCompare(toast, "opened", true)
        wait(2500)
        backupSetController.statusChanged("Second message")
        wait(3000)
        verify(toast.visible, "A new notification must get its own five-second timeout")
        compare(control("notificationMessageLabel").text, "Second message")
        tryCompare(toast, "visible", false, 2500)
    }

    function test_launcherStartedClearsBottomStatus() {
        backupLauncher.failed("Previous launch failed")
        verify(control("notificationToast").visible)
        backupLauncher.started()
        compare(control("notificationMessageLabel").text, "")
        verify(!control("notificationToast").visible)
    }

    function openRecentMenu(index) {
        mouseClick(control("recentActions-" + index))
        const menu = control("recentMenu-" + index)
        tryCompare(menu, "opened", true)
        return menu
    }

    function test_deleteRecentCopyRequiresConfirmationAndCancelIsSafe() {
        compare(openRecentMenu(0).itemAt(0).enabled, false)
        control("recentMenu-0").close()
        mouseClick(openRecentMenu(1).itemAt(0))
        const dialog = control("deleteCopyDialog")
        tryCompare(dialog, "opened", true)
        compare(recentBackupCopies.deleteRequestedId, "documents-id")
        compare(dialog.copyPath, "/backups/documents-id/copy-id")
        verify(dialog.contentItem.text.indexOf("Proton Drive Trash") >= 0)
        if (dashboardScreenshotPath.length > 0) {
            grabImage(app.contentItem).save(dashboardScreenshotPath + ".copy-warning.png")
        }
        compare(recentBackupCopies.deleteConfirmed, false)
        mouseClick(dialog.standardButton(Dialog.Cancel))
        tryCompare(dialog, "visible", false)
        compare(recentBackupCopies.deleteCancelled, true)
        compare(recentBackupCopies.deleteConfirmed, false)
        mouseClick(openRecentMenu(1).itemAt(0))
        tryCompare(dialog, "opened", true)
        mouseClick(dialog.standardButton(Dialog.Ok))
        compare(recentBackupCopies.deleteConfirmed, true)
        tryCompare(control("setBusy-0"), "running", true)
        compare(control("setProgressBar-0").visible, true)
        compare(control("setProgressBar-0").indeterminate, true)
        compare(control("setRemainingTime-0").text, "Deleting backup copy…")
        compare(control("recentDeleteBusy-1").running, true)
        compare(control("recentDeleteStatus-1").visible, true)
        compare(control("setBusy-1").running, false)
        compare(control("recentDeleteBusy-0").running, false)
        compare(control("restore-1").enabled, false)
        recentBackupCopies.busy = false
        recentBackupCopies.deletingSetId = ""
        recentBackupCopies.copyDeleted("documents-id")
        compare(backupSetController.refreshCount, 1)
        tryCompare(control("setBusy-0"), "visible", false)
        compare(control("setProgressBar-0").visible, false)
        compare(control("recentDeleteBusy-1").visible, false)
    }

    function test_runningRecentCopyCannotBeDeleted() {
        backupSetController.runningSetIds = ["documents-id"]
        const menu = openRecentMenu(1)
        compare(menu.itemAt(0).enabled, false)
        menu.close()
    }

    function test_newSetStillOpensTheEditor() {
        mouseClick(control("newBackupSetButton"))
        compare(backupSetController.addedCount, 1)
        compare(app.showEditor, true)
        tryCompare(control("setNameField"), "text", "New set")
    }

    function test_importAndExportUseChosenLocalFiles() {
        compare(control("importSetsButton").text, "Import")
        compare(control("exportSetsButton").text, "Export")
        const exportDialog = control("exportSetsDialog")
        compare(exportDialog.fileMode, FileDialog.SaveFile)
        exportDialog.selectedFile = "file:///tmp/backup%20sets.json"
        exportDialog.accepted()
        compare(backupSetController.exportedPath, "/tmp/backup sets.json")
        mouseClick(control("newBackupSetButton"))
        compare(app.showEditor, true)
        const importDialog = control("importSetsDialog")
        importDialog.selectedFile = dashboardImportFileUrl
        importDialog.accepted()
        tryCompare(control("importModeDialog"), "opened", true)
        compare(backupSetController.importedPath, "")
        mouseClick(control("replaceImportButton"))
        compare(backupSetController.importedPath, dashboardImportFilePath)
        compare(backupSetController.importedMerge, false)
        compare(app.showEditor, false)
    }

    function test_importMergeUsesChosenFile() {
        const dialog = control("importSetsDialog")
        dialog.selectedFile = dashboardImportFileUrl
        dialog.accepted()
        tryCompare(control("importModeDialog"), "opened", true)
        mouseClick(control("mergeImportButton"))
        compare(backupSetController.importedPath, dashboardImportFilePath)
        compare(backupSetController.importedMerge, true)
    }

    function test_templateDownloadUsesChosenFileWithoutClosingEditor() {
        mouseClick(control("newBackupSetButton"))
        compare(control("downloadTemplateButton").text, "Download template")
        const dialog = control("templateDialog")
        compare(dialog.fileMode, FileDialog.SaveFile)
        compare(dialog.defaultSuffix, "json")
        dialog.selectedFile = "file:///tmp/backup%20template.json"
        dialog.accepted()
        compare(backupSetController.templatePath, "/tmp/backup template.json")
        compare(backupSetController.importedPath, "")
        compare(backupSetController.exportedPath, "")
        compare(app.showEditor, true)
    }

    function test_templateDownloadAvailableWithoutBackupSets() {
        backupSetController.setNames = []
        backupSetController.setIds = []
        const menu = control("backupSetsMenu")
        mouseClick(control("backupSetsMenuButton"))
        tryCompare(menu, "opened", true)
        verify(control("downloadTemplateButton").enabled)
        mouseClick(control("downloadTemplateButton"))
        const dialog = control("templateDialog")
        tryCompare(dialog, "visible", true)
        dialog.reject()
        compare(backupSetController.templatePath, "")
    }

    function test_cancelImportDoesNotImportOrCloseEditor() {
        mouseClick(control("newBackupSetButton"))
        const dialog = control("importSetsDialog")
        dialog.selectedFile = dashboardImportFileUrl
        dialog.accepted()
        const mode = control("importModeDialog")
        tryCompare(mode, "opened", true)
        mouseClick(mode.standardButton(Dialog.Cancel))
        tryCompare(mode, "visible", false)
        compare(backupSetController.importedPath, "")
        compare(app.showEditor, true)
    }

    function test_backupSetOptionsOpenBelowTopRightButtonAndDismissWithEscape() {
        const button = control("backupSetsMenuButton")
        const menu = control("backupSetsMenu")
        compare(button.text, "⋯")
        compare(button.Accessible.name, "Backup set options")
        compare(menu.visible, false)
        const position = button.mapToItem(app.contentItem, 0, 0)
        compare(position.x + button.width, app.width - app.contentPadding)
        button.forceActiveFocus()
        keyClick(Qt.Key_Space)
        tryCompare(menu, "opened", true)
        compare(menu.itemAt(0), control("importSetsButton"))
        compare(menu.itemAt(1), control("exportSetsButton"))
        compare(menu.itemAt(2), control("downloadTemplateButton"))
        const menuTop = menu.contentItem.mapToItem(app.contentItem, 0, 0)
        verify(menuTop.y >= position.y + button.height)
        verify(Math.abs(menuTop.x + menu.contentItem.width - position.x - button.width) < 16)
        keyClick(Qt.Key_Escape)
        tryCompare(menu, "visible", false)
        tryCompare(button, "activeFocus", true)
        mouseClick(button)
        tryCompare(menu, "opened", true)
        mouseClick(menu.itemAt(0))
        tryCompare(control("importSetsDialog"), "visible", true)
        control("importSetsDialog").reject()
        mouseClick(button)
        tryCompare(menu, "opened", true)
        mouseClick(menu.itemAt(1))
        tryCompare(control("exportSetsDialog"), "visible", true)
        control("exportSetsDialog").reject()
        mouseClick(button)
        tryCompare(menu, "opened", true)
        mouseClick(menu.itemAt(2))
        tryCompare(control("templateDialog"), "visible", true)
        control("templateDialog").reject()
    }

    function test_actionButtonsMatchOverflowButtonAppearance() {
        const reference = control("backupSetsMenuButton")
        for (const name of ["newBackupSetButton", "setActions-0",
                            "openFolder-1", "restore-1", "recentActions-1"]) {
            const action = control(name)
            verify(action instanceof Button, name + " must use the same button control")
            compare(action.flat, reference.flat)
            compare(action.height, reference.height)
            compare(action.font.pixelSize, reference.font.pixelSize)
            compare(action.palette.buttonText, reference.palette.buttonText)
            verify(action.background !== null)
        }
    }

    function test_failedImportKeepsEditorOpen() {
        mouseClick(control("newBackupSetButton"))
        backupSetController.importSucceeds = false
        const dialog = control("importSetsDialog")
        dialog.selectedFile = dashboardInvalidImportFileUrl
        dialog.accepted()
        tryCompare(control("importModeDialog"), "opened", true)
        mouseClick(control("mergeImportButton"))
        compare(backupSetController.importedPath, app.localPath(dashboardInvalidImportFileUrl))
        compare(app.showEditor, true)
    }

    function test_editorCloseButtonReturnsToDashboard_data() {
        return [
            { tag: "create", creating: true },
            { tag: "edit", creating: false }
        ]
    }

    function test_editorCloseButtonReturnsToDashboard(data) {
        if (data.creating) {
            mouseClick(control("newBackupSetButton"))
        } else {
            mouseClick(openMenu(0).itemAt(0))
        }
        tryCompare(app, "showEditor", true)
        waitForRendering(app.contentItem)
        const close = control("closeEditorButton")
        compare(close.text, "×")
        compare(close.Accessible.name, "Close editor")
        const actions = control("editorActionsRow")
        const buttons = actions.children.filter(function (item) { return item instanceof Button })
        compare(buttons.length, 2)
        compare(buttons[0].text, "Save")
        compare(buttons[1].text, "Preview")
        const texts = visibleTexts(app.contentItem)
        verify(!texts.some(function (text) { return text.startsWith("Give your backup a name") }))
        verify(!texts.some(function (text) { return text.startsWith("Included files (") }))
        verify(!texts.some(function (text) { return text.startsWith("Excluded:") }))
        verify(!texts.some(function (text) { return text.startsWith("Run state:") }))
        const advancedButton = control("advancedSettingsButton")
        const save = control("saveBackupSetButton")
        verify(save.mapToItem(app.contentItem, 0, 0).y
            > advancedButton.mapToItem(app.contentItem, 0, advancedButton.height).y)
        app.showAdvanced = true
        waitForRendering(app.contentItem)
        const advancedPanel = control("advancedSettingsPanel")
        verify(save.mapToItem(app.contentItem, 0, 0).y
            > advancedPanel.mapToItem(app.contentItem, 0, advancedPanel.height).y)
        mouseClick(close)
        tryCompare(app, "showEditor", false)
        compare(backupLauncher.launchedId, "")
    }

    function test_previewDistinguishesAllGroupsWithoutRunningOrSaving_data() {
        return [
            { tag: "mixed results", included: ["/safe/notes.txt"], excluded: ["/safe/cache/output.txt"],
                skipped: ["/safe/link"], missing: ["/safe/missing"] },
            { tag: "only skipped and missing", included: [], excluded: [],
                skipped: ["/safe/unreadable"], missing: ["/safe/missing"] },
            { tag: "empty selection", included: [], excluded: [], skipped: [], missing: [] }
        ]
    }

    function test_previewDistinguishesAllGroupsWithoutRunningOrSaving(data) {
        mouseClick(openMenu(0).itemAt(0))
        compare(control("backupPreview").visible, false)
        backupSetController.previewIncluded = data.included
        backupSetController.previewExcluded = data.excluded
        backupSetController.previewSkipped = data.skipped
        backupSetController.previewMissing = data.missing
        control("scheduleFrequency").currentIndex = 1
        waitForRendering(app.contentItem)
        const scroll = control("editorScrollView").contentItem
        tryVerify(function () { return scroll.contentHeight > scroll.height })
        scroll.contentY = scroll.contentHeight - scroll.height
        waitForRendering(app.contentItem)
        const previewButton = control("previewBackupSetButton")
        const position = previewButton.mapToItem(app.contentItem, 0, 0)
        verify(position.y >= 0 && position.y + previewButton.height <= app.height,
            "Preview button outside viewport: " + position.y + ", scroll: " + scroll.contentY)
        mouseClick(previewButton)
        compare(backupSetController.previewCount, 1)
        tryCompare(control("backupPreview"), "visible", true)
        for (const key of ["included", "excluded", "skipped", "missing"]) {
            const paths = data[key]
            const group = control("previewGroup-" + key)
            compare(group.visible, true)
            compare(group.title, key[0].toUpperCase() + key.slice(1) + " (" + paths.length + ")")
            const list = control("previewPaths-" + key)
            compare(list.count, paths.length)
            compare(list.visible, paths.length > 0)
            const empty = control("previewEmpty-" + key)
            compare(empty.visible, paths.length === 0)
            verify(empty.text.length > 0)
            if (paths.length > 0) {
                tryVerify(function () { return list.itemAtIndex(0) !== null })
                const path = list.itemAtIndex(0)
                compare(path.text, paths[0])
                compare(path.textFormat, Text.PlainText)
                compare(path.readOnly, true)
                compare(path.selectByMouse, true)
            }
        }
        compare(backupSetController.previewCount, 1)
        compare(backupSetController.saveCount, 0)
        compare(backupLauncher.launchedId, "")
        compare(backupScheduler.enableCount, 0)
        compare(resourceUsage.savedIndex, -2)
        if (data.included.length === 0) {
            compare(control("previewEmpty-included").text, "No files will be backed up from this selection.")
        }
    }

    function test_previewFullPathsWrapAndEveryResultCanBeInspected() {
        app.width = app.minimumWidth
        mouseClick(openMenu(0).itemAt(0))
        const paths = []
        for (let index = 0; index < 40; ++index) {
            paths.push("/safe/" + "long-folder-name/".repeat(12) + "file-" + index + "<notes>.txt")
        }
        backupSetController.previewMissing = paths
        backupSetController.preview()
        waitForRendering(app.contentItem)
        const list = control("previewPaths-missing")
        tryCompare(list, "count", 40)
        tryVerify(function () { return list.contentHeight > list.height })
        list.positionViewAtIndex(39, ListView.End)
        tryVerify(function () { return list.itemAtIndex(39) !== null })
        const last = list.itemAtIndex(39)
        compare(last.text, paths[39])
        compare(last.wrapMode, TextEdit.WrapAnywhere)
        verify(last.height > 2 * last.font.pixelSize)
        verify(last.width <= list.width)
        last.selectAll()
        compare(last.selectedText, paths[39])
    }

    function test_keyboardMenuNavigationAndEscape() {
        const button = control("setActions-1")
        compare(button.Accessible.name, "Actions for Photos")
        button.forceActiveFocus()
        keyClick(Qt.Key_Space)
        const menu = control("setMenu-1")
        tryCompare(menu, "opened", true)
        keyClick(Qt.Key_Escape)
        tryCompare(menu, "visible", false)
        tryCompare(button, "activeFocus", true)
        keyClick(Qt.Key_Space)
        tryCompare(menu, "opened", true)
        keyClick(Qt.Key_Down)
        keyClick(Qt.Key_Return)
        tryCompare(app, "showEditor", true)
        compare(backupSetController.currentIndex, 1)
    }

    function test_sourcePickerMenuOpensBelowPlusButton() {
        mouseClick(control("newBackupSetButton"))
        tryCompare(app, "showEditor", true)
        waitForRendering(app.contentItem)
        const button = control("addSourceButton")
        mouseClick(button)
        const menu = control("sourceMenu")
        tryCompare(menu, "opened", true)
        compare(menu.parent, button)
        compare(menu.itemAt(0).text, "Add files")
        compare(menu.itemAt(1).text, "Add folder")
        const buttonBottom = button.mapToItem(app.contentItem, 0, button.height)
        const menuTop = menu.contentItem.mapToItem(app.contentItem, 0, 0)
        verify(menuTop.y >= buttonBottom.y, "Source choices must open below the + button")
        verify(Math.abs(menuTop.x + menu.contentItem.width - buttonBottom.x - button.width) < 16)
        menu.close()
    }

    function test_outsideClickDismissesMenu() {
        const menu = openMenu(0)
        mouseClick(app.contentItem, app.width - 24, app.height - 24)
        tryCompare(menu, "visible", false)
        compare(backupLauncher.launchedId, "")
        compare(backupSetController.removedIndex, -1)
    }

    function test_emptyDashboardKeepsCreationAvailable() {
        backupSetController.setIds = []
        backupSetController.setNames = []
        backupSetController.recentBackupSetIds = []
        backupSetController.recentBackupTimestamps = []
        backupSetController.recentBackups = []
        tryCompare(control("dashboardSetsList"), "count", 0)
        tryCompare(control("recentBackupsList"), "count", 0)
        verify(control("emptySetsLabel").visible)
        verify(control("emptyRecentLabel").visible)
        verify(control("newBackupSetButton").enabled)
        verify(control("importSetsButton").enabled)
        verify(!control("exportSetsButton").enabled)
    }

    function test_minimumWindowAndLongHistoryStayWithinColumns() {
        app.width = app.minimumWidth
        app.height = app.minimumHeight
        backupSetController.recentBackups = ["Photos\nfailed | " + "VeryLongUnbrokenFailureMessage".repeat(8), "Documents: succeeded"]
        waitForRendering(app.contentItem)
        const sets = control("dashboardSetsList")
        const recent = control("recentBackupsList")
        verify(sets.width >= 200)
        verify(Math.abs(recent.width - 2 * sets.width) < 2)
        const restore = control("restore-0")
        const position = restore.mapToItem(app.contentItem, 0, 0)
        verify(position.x + restore.width <= app.width - app.contentPadding)
        verify(position.y + restore.height <= app.height)
        const summary = control("recentSummary-0")
        compare(summary.wrapMode, Text.NoWrap)
        compare(summary.maximumLineCount, 1)
        compare(summary.elide, Text.ElideRight)
        verify(summary.text.indexOf("\n") < 0)
        verify(summary.implicitHeight < 40)
        verify(summary.truncated)
        if (dashboardScreenshotPath.length > 0) {
            grabImage(app.contentItem).save(dashboardScreenshotPath + ".minimum.png")
        }
    }

    function test_manySetsScrollAndLastMenuKeepsItsTarget() {
        app.width = app.minimumWidth
        app.height = app.minimumHeight
        const names = ["Documents", "Photos"]
        const ids = ["documents-id", "photos-id"]
        for (let index = 2; index < 15; ++index) {
            names.push("Backup " + index)
            ids.push("backup-id-" + index)
        }
        backupSetController.setIds = ids
        backupSetController.setNames = names
        const list = control("dashboardSetsList")
        tryCompare(list, "count", 15)
        // Component/layout bindings settle after the window resize and model update.
        waitForRendering(app.contentItem)
        list.forceLayout()
        verify(list.contentHeight > list.height)
        verify(list.height <= 360)
        list.positionViewAtIndex(14, ListView.Contain)
        waitForRendering(app.contentItem)
        const button = control("setActions-14")
        const position = button.mapToItem(list, 0, 0)
        verify(position.y >= 0)
        verify(position.y + button.height <= list.height,
            "Last action must fit: y=" + position.y + ", button=" + button.height + ", list=" + list.height)
        mouseClick(openMenu(14).itemAt(1))
        compare(backupLauncher.launchedId, "backup-id-14")
    }

    function test_signInIsShownOnlyWhenDisconnected() {
        const signIn = control("protonSignInButton")
        compare(signIn.visible, false)
        protonAuth.authenticated = false
        protonAuth.error = "Not authenticated. Run proton-drive auth login."
        tryCompare(signIn, "visible", true)
        compare(control("protonErrorLabel").text, protonAuth.error)
        waitForRendering(app.contentItem)
        mouseClick(signIn)
        compare(protonAuth.signInCount, 1)
        protonAuth.checking = true
        tryCompare(signIn, "enabled", false)
        mouseClick(signIn)
        compare(protonAuth.signInCount, 1)
        protonAuth.checking = false
        const refreshCount = protonAuth.refreshCount
        waitForRendering(app.contentItem)
        mouseClick(control("protonAuthRetryButton"))
        compare(protonAuth.refreshCount, refreshCount + 1)
        protonAuth.authenticated = true
        protonAuth.error = ""
        tryCompare(signIn, "visible", false)
        compare(control("protonAuthRetryButton").visible, false)
        compare(control("protonErrorRow").visible, false)
    }

    function test_missingCliShowsInstallationHelpInsteadOfSignIn() {
        protonAuth.authenticated = false
        protonAuth.cliAvailable = false
        protonAuth.error = "Cannot find the Proton Drive CLI. Install proton-drive and retry."
        tryCompare(control("protonErrorRow"), "visible", true)
        compare(control("protonErrorLabel").text, protonAuth.error)
        compare(control("protonSignInButton").visible, false)
        const refreshCount = protonAuth.refreshCount
        mouseClick(control("protonAuthRetryButton"))
        compare(protonAuth.refreshCount, refreshCount + 1)
    }

    function test_initialChecksAndHealthyStatesStayQuiet() {
        protonAuth.authenticated = false
        protonAuth.checked = false
        protonAuth.checking = true
        backupScheduler.ready = false
        backupScheduler.busy = true
        backupScheduler.status = "Checking scheduling…"
        compare(control("protonErrorRow").visible, false)
        compare(control("schedulingErrorRow").visible, false)
        compare(findChild(app, "schedulingStatusLabel"), null)
        compare(findChild(app, "checkSchedulingButton"), null)
        protonAuth.statusChanged("Signed in to Proton Drive.")
        backupScheduler.messageChanged("Backup settings saved. Scheduling is active.")
        compare(control("notificationToast").visible, false)
    }

    function test_schedulingErrorsStayVisibleAndCanBeRetried() {
        compare(control("schedulingErrorRow").visible, false)
        compare(control("enableSchedulingButton").visible, false)
        backupScheduler.ready = false
        backupScheduler.status = "Scheduling paused"
        backupScheduler.error = "Scheduled backups are paused. Enable scheduling to resume them."
        tryCompare(control("enableSchedulingButton"), "visible", true)
        waitForRendering(app.contentItem)
        mouseClick(control("enableSchedulingButton"))
        compare(backupScheduler.enableCount, 1)
        backupScheduler.busy = true
        compare(control("enableSchedulingButton").enabled, false)
        backupScheduler.busy = false
        backupScheduler.error = "Unit omacustos.timer is masked"
        backupScheduler.status = "Scheduling needs attention"
        const error = control("schedulingErrorLabel")
        compare(error.visible, true)
        compare(error.text, "Unit omacustos.timer is masked")
        backupScheduler.failed("Could not activate scheduling")
        compare(control("notificationMessageLabel").text, "Could not activate scheduling")
        backupScheduler.hasSchedules = false
        compare(control("enableSchedulingButton").visible, false)
        backupScheduler.error = ""
        backupScheduler.ready = true
        tryCompare(control("schedulingErrorRow"), "visible", false)
    }

    function test_resourceUsageOffersFiveGlobalPresetsAndSavesSelection() {
        mouseClick(openMenu(0).itemAt(0))
        app.showAdvanced = true
        const preset = control("resourceUsagePreset")
        const defaults = control("useSystemResourceDefaults")
        compare(preset.count, 5)
        compare(preset.model, ["Very low", "Low", "Medium", "High", "Very high"])
        compare(preset.currentIndex, 0)
        compare(defaults.checked, true)
        compare(preset.enabled, false)
        verify(control("resourceUsageDescription").text.indexOf("No CPU cap") >= 0)
        const save = control("saveBackupSetButton")
        save.clicked()
        compare(resourceUsage.savedIndex, -1)
        defaults.checked = false
        compare(preset.enabled, true)
        preset.currentIndex = 4
        compare(control("resourceUsageDescription").text, "CPU limit: 200%")
        save.clicked()
        compare(resourceUsage.savedIndex, 4)
        defaults.checked = true
        save.clicked()
        compare(resourceUsage.savedIndex, -1)
        resourceUsage.busy = true
        compare(save.enabled, false)
        compare(preset.enabled, false)
        compare(defaults.enabled, false)
    }

    function test_themePaletteAndLiveChanges() {
        compare(app.palette.window, "#ffffff")
        compare(app.palette.windowText, "#19232e")
        compare(app.palette.disabled.buttonText, "#586575")
        compare(control("newBackupSetButton").palette.buttonText, "#19232e")
        compare(control("restore-0").palette.disabled.buttonText, "#586575")
        const image = grabImage(app.contentItem)
        compare(image.pixel(app.width - 8, app.height - 8), "#ffffff")
        if (dashboardScreenshotPath.length > 0) {
            image.save(dashboardScreenshotPath)
        }
        themeColors.colors = {
            background: "#060606", foreground: "#f0d4e4", muted: "#626262",
            surface: "#1f1f1e", border: "#393339", accent: "#9b6b9f",
            highlight: "#f0d4e4", highlightedText: "#060606", brightText: "#f4deea"
        }
        tryCompare(app.palette, "window", "#060606")
        compare(app.palette.windowText, "#f0d4e4")
        compare(app.palette.highlight, "#f0d4e4")
        compare(app.palette.highlightedText, "#060606")
        tryCompare(app.palette.disabled, "buttonText", "#626262")
        compare(control("newBackupSetButton").palette.buttonText, "#f0d4e4")
        tryCompare(control("restore-0").palette.disabled, "buttonText", "#626262")
        waitForRendering(app.contentItem)
        const darkImage = grabImage(app.contentItem)
        compare(darkImage.pixel(app.width - 8, app.height - 8), "#060606")
    }
}

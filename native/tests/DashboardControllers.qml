import QtQuick

QtObject {
    property QtObject backupScheduler: QtObject {
        property bool busy: false
        property bool ready: true
        property bool hasSchedules: true
        property string status: "Scheduling active"
        property string error: ""
        property int enableCount: 0
        property int refreshCount: 0
        signal messageChanged(string message)
        signal failed(string error)
        function enable() { enableCount++ }
        function refresh() { refreshCount++ }
    }

    property QtObject resourceUsage: QtObject {
        property var names: ["Very low", "Low", "Medium", "High", "Very high"]
        property var descriptions: ["CPU limit: 10%", "CPU limit: 25%", "CPU limit: 50%", "CPU limit: 100%", "CPU limit: 200%"]
        property int presetIndex: -1
        property bool busy: false
        property int savedIndex: -2
        signal presetChanged()
        signal statusChanged(string message)
        signal failed(string error)
        function save(index) { savedIndex = index }
    }

    property QtObject protonAuth: QtObject {
        property bool authenticated: true
        property bool checked: true
        property bool checking: false
        property bool cliAvailable: true
        property string error: ""
        property int signInCount: 0
        property int refreshCount: 0
        signal statusChanged(string message)
        signal failed(string error)
        function signIn() { signInCount++ }
        function refresh() { refreshCount++ }
    }

    property QtObject themeColors: QtObject {
        property var colors: ({
            background: "#ffffff", foreground: "#19232e", muted: "#586575",
            surface: "#f0f3f6", border: "#dce1e7", accent: "#245bcb",
            highlight: "#245bcb", highlightedText: "#ffffff", brightText: "#ffffff"
        })
    }

    property QtObject backupSetController: QtObject {
        property var setNames: ["Documents", "Photos"]
        property var setIds: ["documents-id", "photos-id"]
        property int currentIndex: 0
        property string currentId: setIds[currentIndex] || ""
        property string currentName: setNames[currentIndex] || ""
        property string currentRemoteRoot: "/backups/" + currentId
        property var currentSources: ["/safe/" + currentId]
        property var currentExclusions: []
        property string currentScheduleFrequency: "disabled"
        property int currentScheduleHour: 2
        property int currentScheduleMinute: 0
        property int currentScheduleWeekday: 1
        property int currentScheduleDayOfMonth: 1
        property int currentRetention: 3
        property bool currentOnlyOnAcPower: false
        property string currentStagingDirectory: ""
        property double currentStagingBudget: 1000000000
        property var currentRequiredMounts: []
        property string currentNextRun: "Disabled"
        property string currentRunStatus: "idle"
        property string currentRunError: ""
        property var runningSetIds: []
        property var remainingTimes: ({})
        property var transferProgress: ({})
        property var runDetails: ({})
        property var setSummaries: ({})
        readonly property var runSummaries: runDetails
        onRunDetailsChanged: runStateChanged()
        property var recentBackups: ["Photos\nNo backup run yet", "Documents\nsucceeded"]
        property var recentBackupSetIds: ["photos-id", "documents-id"]
        property var recentBackupTimestamps: ["", "01/10/2026 10:00:00"]
        property bool previewAvailable: false
        property bool previewBusy: false
        property string dashboardRefreshError: ""
        property int previewCount: 0
        property int saveCount: 0
        property int appliedDraftCount: 0
        property var lastDraft: ({})
        property var previewIncluded: []
        property var previewExcluded: []
        property var previewSkipped: []
        property var previewMissing: []
        property var cleanupTargets: []
        property bool cleanupConfirmationRequired: false
        property int removedIndex: -1
        property int addedCount: 0
        property int refreshCount: 0
        property string importedPath: ""
        property bool importedMerge: false
        property string exportedPath: ""
        property string templatePath: ""
        property bool importSucceeds: true
        property bool saveSucceeds: true
        property var unsavedSetIds: []
        signal currentSetChanged()
        signal runStateChanged()
        signal statusChanged(string status)
        signal failed(string error)
        onCurrentIndexChanged: {
            previewAvailable = false
            // Let dependent QML bindings settle before mirroring the C++ signal.
            Qt.callLater(currentSetChanged)
        }
        function removeSet(index) { removedIndex = index }
        function removeCurrentSet() { removeSet(currentIndex) }
        function addSet() {
            addedCount++
            setIds = setIds.concat(["new-id"])
            setNames = setNames.concat(["New set"])
            currentIndex = setNames.length - 1
            unsavedSetIds = unsavedSetIds.concat([currentId])
        }
        function discardUnsavedSet() {
            if (unsavedSetIds.indexOf(currentId) < 0) return
            const id = currentId
            setIds = setIds.filter(function (candidate) { return candidate !== id })
            setNames = setNames.filter(function (name, index) { return index !== currentIndex })
            unsavedSetIds = unsavedSetIds.filter(function (candidate) { return candidate !== id })
            currentIndex = Math.min(currentIndex, setNames.length - 1)
            previewBusy = false
            previewAvailable = false
        }
        function preview() { previewCount++; previewAvailable = true }
        function applyCurrentDraft(draft) {
            appliedDraftCount++
            lastDraft = draft
            currentName = draft.name
            currentRemoteRoot = draft.remoteRoot
            currentSources = draft.sources.map(function (path) { return path.trim() })
                .filter(function (path) { return path.length > 0 })
            currentExclusions = draft.exclusions
            currentScheduleFrequency = draft.scheduleFrequency
            currentScheduleHour = draft.scheduleHour
            currentScheduleMinute = draft.scheduleMinute
            currentScheduleWeekday = draft.scheduleWeekday
            currentScheduleDayOfMonth = draft.scheduleDayOfMonth
            currentRetention = draft.retention
            currentOnlyOnAcPower = draft.onlyOnAcPower
            currentStagingDirectory = draft.stagingDirectory
            currentStagingBudget = draft.stagingBudget
            currentSetChanged()
        }
        function recentBackupFolderPath(setId) {
            return setIds.indexOf(setId) >= 0 ? "/backups/" + setId : ""
        }
        function save() {
            saveCount++
            if (saveSucceeds) unsavedSetIds = []
            return saveSucceeds
        }
        function importSets(path, merge) {
            importedPath = path
            importedMerge = merge
            if (importSucceeds) {
                discardUnsavedSet()
                unsavedSetIds = []
            }
            return importSucceeds
        }
        function exportSets(path) { exportedPath = path; return true }
        function saveTemplate(path) { templatePath = path; return true }
        function confirmCleanup() { return true }
        function refreshRunState() { refreshCount++ }
        function backupDetails(setId) { return runDetails[setId] || ({}) }
    }

    property QtObject backupLauncher: QtObject {
        property string launchedId: ""
        property string pausedId: ""
        property string resumedId: ""
        property string cancelledId: ""
        signal started()
        signal failed(string error)
        function startBackup(id) { launchedId = id }
        function pauseBackup(id) { pausedId = id }
        function resumeBackup(id) { resumedId = id }
        function cancelBackup(id) { cancelledId = id }
    }

    property QtObject protonFolderBrowser: QtObject {
        property bool busy: false
        property string requestedPath: ""
        signal folderResolved(url folderUrl)
        signal failed(string error)
        function openFolder(path) { requestedPath = path }
    }

    property QtObject recentBackupCopies: QtObject {
        property bool busy: false
        property string deletingSetId: ""
        property string openedId: ""
        property string deleteRequestedId: ""
        property bool deleteConfirmed: false
        property bool deleteCancelled: false
        signal folderResolved(string path)
        signal deleteConfirmationReady(string name, string path)
        signal copyDeleted(string setId)
        signal statusChanged(string message)
        signal failed(string error)
        function openCopy(setId) { openedId = setId }
        function requestDelete(setId) {
            deleteRequestedId = setId
            deleteConfirmationReady("Documents", "/backups/documents-id/copy-id")
        }
        function confirmDelete() {
            deleteConfirmed = true
            deletingSetId = deleteRequestedId
            busy = true
        }
        function cancelDelete() { deleteCancelled = true }
    }

    property QtObject restoreController: QtObject {
        property bool busy: false
        property bool restoring: false
        readonly property bool browsing: busy && !restoring
        readonly property string backupFolder: discoveredRoot
        readonly property string backupId: discoveredSetId
        readonly property string currentCopyPath: currentCopyIndex >= 0 ? discoveredRoot + "/copy-" + currentCopyIndex : ""
        property string browseError: ""
        property string restoreProgress: ""
        property real restoreProgressFraction: 0
        property string restoreBackupFolder: ""
        property string restoreBackupId: ""
        property string restoreCopyPath: ""
        property string loadingMessage: ""
        property bool showingCachedData: false
        property bool verified: false
        readonly property bool restoreEligible: verified && !busy
        property int currentCopyIndex: -1
        property int selectedCopy: -1
        property var entries: []
        property var copies: []
        property var unavailableEntries: []
        property string copySearch: ""
        property string defaultDestination: "/safe/restore"
        property string discoveredRoot: ""
        property string discoveredSetId: ""
        property var restoredIndexes: []
        property string restoreDestination: ""
        property int restoreCount: 0
        property bool restoreSucceeds: true
        signal statusChanged(string status)
        signal failed(string error)
        signal restoreCompleted()
        signal restoreCompletedForContext(string folder, string setId, string copyPath)
        function discover(remoteRoot, setId) { discoveredRoot = remoteRoot; discoveredSetId = setId }
        function selectCopy(index) { selectedCopy = index; currentCopyIndex = index }
        function restoreSelected(indexes, destination) {
            restoreBackupFolder = backupFolder
            restoreBackupId = backupId
            restoreCopyPath = currentCopyPath
            restoredIndexes = indexes.slice()
            restoreDestination = destination
            restoreCount++
            if (restoreSucceeds) {
                restoreCompleted()
                restoreCompletedForContext(restoreBackupFolder, restoreBackupId, restoreCopyPath)
            } else {
                failed("Restore failed")
            }
        }
        function restoreFolder(folder, destination) {}
    }
}

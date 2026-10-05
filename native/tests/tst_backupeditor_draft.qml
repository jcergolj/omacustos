import QtQuick
import QtTest
import "../qml" as OmaCustos

TestCase {
    id: testCase
    name: "BackupEditorDraft"
    when: windowShown

    Component { id: windowComponent; OmaCustos.Main {} }

    function test_saveAndPreviewApplyOneCompleteDraft_data() {
        return [
            { tag: "preview", action: "preview", succeeds: true },
            { tag: "save", action: "save", succeeds: true },
            { tag: "failed save", action: "save", succeeds: false }
        ]
    }

    function test_saveAndPreviewApplyOneCompleteDraft(data) {
        backupSetController.currentIndex = 0
        backupSetController.currentName = "Documents"
        backupSetController.currentRemoteRoot = "/custom-backups"
        backupSetController.currentSources = ["/safe/documents"]
        backupSetController.currentExclusions = ["cache"]
        backupSetController.currentScheduleFrequency = "weekly"
        backupSetController.currentScheduleHour = 9
        backupSetController.currentScheduleMinute = 30
        backupSetController.currentScheduleWeekday = 4
        backupSetController.currentScheduleDayOfMonth = 12
        backupSetController.currentRetention = 7
        backupSetController.currentOnlyOnAcPower = true
        backupSetController.appliedDraftCount = 0
        backupSetController.previewCount = 0
        backupSetController.previewBusy = false
        backupSetController.previewAvailable = false
        backupSetController.saveCount = 0
        backupSetController.saveSucceeds = data.succeeds
        resourceUsage.presetIndex = 2
        resourceUsage.savedIndex = -2
        resourceUsage.busy = false
        backupScheduler.enableCount = 0
        const app = createTemporaryObject(windowComponent, testCase)
        verify(app !== null)
        app.editSet(0)
        waitForRendering(app.contentItem)
        const editor = findChild(app, "editorScrollView")
        const name = findChild(app, "setNameField")
        name.text = "Updated documents"
        editor.addSource("file:///safe/notes.txt")
        editor.addExclusion("file:///safe/private")
        findChild(app, "scheduleFrequency").currentIndex = 3

        findChild(app, data.action === "save" ? "saveBackupSetButton" : "previewBackupSetButton").clicked()

        compare(backupSetController.appliedDraftCount, 1)
        compare(backupSetController.lastDraft, {
            name: "Updated documents", remoteRoot: "/custom-backups",
            sources: ["/safe/documents", "/safe/notes.txt"], exclusions: ["cache", "/safe/private"],
            scheduleFrequency: "monthly", scheduleHour: 9, scheduleMinute: 30,
            scheduleWeekday: 4, scheduleDayOfMonth: 12, retention: 7, onlyOnAcPower: true,
            stagingDirectory: "", stagingBudget: 1000000000
        })
        compare(name.text, "Updated documents")
        compare(editor.syncingCurrentSet, false)
        compare(backupSetController.saveCount, data.action === "save" ? 1 : 0)
        compare(backupSetController.previewCount, data.action === "preview" ? 1 : 0)
        compare(resourceUsage.savedIndex, data.action === "save" && data.succeeds ? 2 : -2)
        compare(backupScheduler.enableCount, 0)
    }
}

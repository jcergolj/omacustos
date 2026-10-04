import QtQuick

QtObject {
    id: selection

    // Arrays and lookup are owned JS containers, never independently assignable.
    // Notify only after metadata and every selection-derived value agree.
    readonly property var selectedIndexes: []
    readonly property var selectedPaths: []
    readonly property var selectionLookup: Object.create(null)
    property var _state: ({
        entries: [],
        folderRows: [], folders: Object.create(null), ancestors: [],
        initialized: false, customized: false, copyPath: "", copyIndex: -1
    })
    property int _revision: 0
    readonly property int selectionRevision: _revision
    readonly property int selectedCount: { _revision; return selectedIndexes.length }
    readonly property var folderRows: _state.folderRows
    readonly property bool initialized: { _revision; return _state.initialized }
    readonly property bool customized: { _revision; return _state.customized }
    readonly property string copyPath: _state.copyPath
    readonly property int copyIndex: _state.copyIndex

    function folderStructure(entries) {
        const folders = Object.create(null)
        const ancestors = []
        entries.forEach(function (path) {
            const paths = []
            let end = path.lastIndexOf("/")
            while (end > 0) {
                const folder = path.slice(0, end)
                if (!folders[folder]) folders[folder] = { total: 0, selected: 0 }
                ++folders[folder].total
                paths.push(folder)
                end = folder.lastIndexOf("/")
            }
            ancestors.push(paths)
        })
        return { folders: folders, ancestors: ancestors, folderRows: Object.keys(folders).sort() }
    }

    function replaceSelection(entries, indexes, initialized, customized, copyPath, copyIndex, structure) {
        selectedIndexes.length = 0
        selectedPaths.length = 0
        for (const index of Object.keys(selectionLookup)) delete selectionLookup[index]
        for (const path of structure.folderRows) structure.folders[path].selected = 0
        indexes.forEach(function (index, position) {
            selectionLookup[index] = position
            selectedIndexes.push(index)
            selectedPaths.push(entries[index])
            for (const folder of structure.ancestors[index]) ++structure.folders[folder].selected
        })
        _state = {
            entries: entries,
            folderRows: structure.folderRows, folders: structure.folders, ancestors: structure.ancestors,
            initialized: initialized, customized: customized, copyPath: copyPath, copyIndex: copyIndex
        }
        ++_revision
        selectedIndexesChanged()
        selectedPathsChanged()
    }

    function reset() {
        replaceSelection([], [], false, false, "", -1, folderStructure([]))
    }

    function syncCopy(path, index, browsing) {
        if (_state.copyPath !== path && !(path.length === 0 && browsing)) {
            replaceSelection(_state.entries, [], false, false, path, index, _state)
        } else if (_state.copyIndex !== index) {
            _state = Object.assign({}, _state, { copyIndex: index })
        }
    }

    function reconcileEntries(entries, browsing) {
        if (entries.length === 0 && browsing) return
        const ownedEntries = entries.slice()
        const indexes = []
        const useDefault = !_state.initialized || !_state.customized
        if (useDefault) {
            ownedEntries.forEach(function (path, index) { indexes.push(index) })
        } else {
            const indexesByPath = Object.create(null)
            ownedEntries.forEach(function (path, index) { indexesByPath[path] = index })
            for (const path of selectedPaths) {
                const index = indexesByPath[path]
                if (index !== undefined) indexes.push(index)
            }
        }
        replaceSelection(ownedEntries, indexes, _state.initialized || entries.length > 0,
            _state.customized, _state.copyPath, _state.copyIndex, folderStructure(ownedEntries))
    }

    // Coherent replacement seam for callers/tests that previously assigned the
    // index and path arrays separately. Invalid and duplicate indexes are ignored.
    function setIndexes(indexes) {
        const valid = []
        const seen = Object.create(null)
        for (const index of indexes) {
            if (!Number.isInteger(index) || index < 0 || index >= _state.entries.length || seen[index]) continue
            seen[index] = true
            valid.push(index)
        }
        replaceSelection(_state.entries, valid, true, true, _state.copyPath, _state.copyIndex, _state)
    }

    function selectFolder(path, checked) {
        const prefix = path.length > 0 ? path + "/" : ""
        const indexes = []
        _state.entries.forEach(function (entry, index) {
            if (entry.startsWith(prefix) ? checked : selectionLookup[index] !== undefined) indexes.push(index)
        })
        replaceSelection(_state.entries, indexes, true, true, _state.copyPath, _state.copyIndex, _state)
    }

    function selectAll() { selectFolder("", true) }
    function clear() { selectFolder("", false) }

    function toggleFile(index, checked) {
        if (!Number.isInteger(index) || index < 0 || index >= _state.entries.length) return
        const position = selectionLookup[index]
        if (checked === (position !== undefined)) return
        _state.initialized = true
        _state.customized = true
        if (checked) {
            selectionLookup[index] = selectedIndexes.length
            selectedIndexes.push(index)
            selectedPaths.push(_state.entries[index])
        } else {
            // Selection order is immaterial. Swap removal is constant time.
            const last = selectedIndexes.length - 1
            const movedIndex = selectedIndexes[last]
            selectedIndexes[position] = movedIndex
            selectedPaths[position] = selectedPaths[last]
            selectionLookup[movedIndex] = position
            selectedIndexes.pop()
            selectedPaths.pop()
            delete selectionLookup[index]
        }
        for (const folder of _state.ancestors[index]) _state.folders[folder].selected += checked ? 1 : -1
        ++_revision
        selectedIndexesChanged()
        selectedPathsChanged()
    }

    function folderCheckState(path) {
        const folder = _state.folders[path]
        return !folder || folder.selected === 0 ? Qt.Unchecked
            : folder.selected === folder.total ? Qt.Checked : Qt.PartiallyChecked
    }

    function snapshot() {
        return {
            entries: _state.entries.slice(), indexes: selectedIndexes.slice(),
            initialized: _state.initialized, customized: _state.customized,
            copyPath: _state.copyPath, copyIndex: _state.copyIndex
        }
    }

    function restoreSnapshot(snapshot) {
        const entries = snapshot.entries.slice()
        replaceSelection(entries, snapshot.indexes.slice(), snapshot.initialized, snapshot.customized,
            snapshot.copyPath, snapshot.copyIndex, folderStructure(entries))
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

GroupBox {
    id: group
    required property var style
    required property string key
    required property string heading
    required property string emptyText
    required property var paths
    property string summary: ""
    objectName: "previewGroup-" + key
    title: summary.length > 0
        ? qsTr("%1 (%2) — %3").arg(heading).arg(paths.length).arg(summary)
        : qsTr("%1 (%2)").arg(heading).arg(paths.length)
    Layout.fillWidth: true

    ColumnLayout {
        anchors.fill: parent

        Label {
            objectName: "previewEmpty-" + group.key
            text: group.emptyText
            visible: group.paths.length === 0
            font.pixelSize: group.style.metadataTypeSize
            color: group.style.mutedColor
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        ListView {
            id: pathsList
            objectName: "previewPaths-" + group.key
            model: group.paths
            visible: count > 0
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(160, contentHeight)
            clip: true
            ScrollBar.vertical: ScrollBar {}
            delegate: TextArea {
                required property int index
                required property string modelData
                objectName: "previewPath-" + group.key + "-" + index
                text: modelData
                textFormat: Text.PlainText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                font.pixelSize: group.style.metadataTypeSize
                width: pathsList.width
                padding: 0
                background: null
            }
        }
    }
}

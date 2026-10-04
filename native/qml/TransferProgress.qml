import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: progress
    required property var style
    property string statusText: ""
    property string detailText: ""
    property real fraction: 0
    property bool indeterminate: true
    property string accessibleName: ""
    property string statusObjectName: ""
    property string detailObjectName: ""
    property string barObjectName: ""
    spacing: 10

    Label {
        objectName: progress.statusObjectName
        visible: text.length > 0
        text: progress.statusText
        textFormat: Text.PlainText
        color: progress.style.mutedColor
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }
    Label {
        objectName: progress.detailObjectName
        visible: text.length > 0
        text: progress.detailText
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }
    ProgressBar {
        objectName: progress.barObjectName
        value: progress.fraction
        indeterminate: progress.indeterminate
        Accessible.name: progress.accessibleName
        Layout.fillWidth: true
    }
}

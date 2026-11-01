import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Button {
    id: button
    required property var style
    property string disabledReason: ""
    Accessible.description: !enabled ? disabledReason : ""
    readonly property bool pressFeedback: enabled && (down || feedbackTimer.running)
    font.family: style.bodyFontFamily
    font.pixelSize: style.bodyTypeSize
    font.weight: Font.Normal
    leftPadding: style.buttonHorizontalPadding
    rightPadding: style.buttonHorizontalPadding
    topPadding: style.buttonVerticalPadding
    bottomPadding: style.buttonVerticalPadding
    implicitHeight: Math.max(style.buttonHeight, implicitBackgroundHeight + topInset + bottomInset,
        implicitContentHeight + topPadding + bottomPadding)
    Layout.preferredHeight: implicitHeight

    scale: pressFeedback ? 0.97 : 1
    Behavior on scale {
        NumberAnimation { duration: button.pressFeedback ? 60 : 120 }
    }

    onPressedChanged: {
        if (pressed) feedbackTimer.restart()
    }

    Timer {
        id: feedbackTimer
        interval: 160
    }

    // Preserve the platform's enabled button appearance. Disabled controls get
    // a distinct patterned surface rather than relying on subtle text colour.
    Rectangle {
        id: disabledSurface
        objectName: button.objectName + "-disabledSurface"
        visible: !button.enabled
        anchors.fill: parent
        z: -0.5
        radius: 6
        clip: true
        color: Qt.tint(button.style.backgroundColor, Qt.rgba(button.style.mutedColor.r,
            button.style.mutedColor.g, button.style.mutedColor.b, 0.22))
        border.color: button.style.mutedColor
        border.width: 1
        Repeater {
            model: disabledSurface.visible ? Math.ceil(parent.width / 10) + 8 : 0
            Rectangle {
                required property int index
                x: index * 10 - 40
                anchors.verticalCenter: parent.verticalCenter
                width: 1
                height: parent.height * 3
                rotation: 35
                color: disabledSurface.border.color
                opacity: 0.2
            }
        }
    }

    Binding {
        target: button.contentItem
        property: "opacity"
        value: 0.65
        when: !button.enabled
        restoreMode: Binding.RestoreBindingOrValue
    }

    Rectangle {
        objectName: button.objectName + "-disabledIndicator"
        visible: !button.enabled
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: -3
        width: 16
        height: 16
        radius: 8
        color: button.style.mutedColor
        border.color: button.style.inkColor
        z: 2
        Label {
            anchors.centerIn: parent
            text: "⊘"
            color: button.style.backgroundColor
            font.pixelSize: 13
            Accessible.ignored: true
        }
    }

    Rectangle {
        anchors.fill: parent
        z: 1
        radius: 6
        color: button.style.accentColor
        border.color: button.style.accentColor
        border.width: 1
        opacity: button.pressFeedback ? 0.24 : 0
        Behavior on opacity {
            NumberAnimation { duration: button.pressFeedback ? 60 : 120 }
        }
    }
}

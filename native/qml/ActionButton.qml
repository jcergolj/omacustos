import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Button {
    id: button
    required property var style
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

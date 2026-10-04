import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Button {
    required property var style
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
}

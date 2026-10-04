import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Frame {
    id: card
    required property var style
    default property alias contents: formContent.data
    padding: style.cardPadding
    font.family: style.bodyFontFamily
    font.pixelSize: style.bodyTypeSize
    font.weight: Font.Normal
    implicitHeight: formContent.implicitHeight + topPadding + bottomPadding

    background: Rectangle {
        color: card.style.backgroundColor
        radius: 10
        border.color: card.style.lineColor
    }

    contentItem: ColumnLayout {
        id: formContent
        spacing: 20
    }
}

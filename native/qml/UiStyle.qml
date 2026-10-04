import QtQuick

QtObject {
    required property var colors
    property string systemFontFamily: Qt.application.font.family
    property string displayFontFamily: systemFontFamily
    property string bodyFontFamily: systemFontFamily
    property int displayTypeSize: 20
    property int pageTitleSize: 24
    property int sectionTitleSize: 16
    property int bodyTypeSize: 14
    property int metadataTypeSize: 12
    property real bodyLeading: 1.4
    property int readableMeasure: 680
    property int contentPadding: 24
    property int cardPadding: 20
    property int buttonHorizontalPadding: 12
    property int buttonVerticalPadding: 8
    property int buttonHeight: 36
    property int buttonSpacing: 12
    readonly property color backgroundColor: colors.background
    readonly property color inkColor: colors.foreground
    readonly property color mutedColor: colors.muted
    readonly property color lineColor: colors.border
    readonly property color softColor: colors.surface
    readonly property color accentColor: colors.accent
}

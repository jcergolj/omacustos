import QtQuick
import QtQuick.Controls

Dialog {
    id: dialog
    required property var style

    footer: DialogButtonBox {
        standardButtons: dialog.standardButtons
        visible: count > 0
        spacing: dialog.style.buttonSpacing
        delegate: ActionButton {
            style: dialog.style
        }
        onAccepted: dialog.accept()
        onRejected: dialog.reject()
    }
}

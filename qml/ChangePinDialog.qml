import QtQuick 2.0
import QtQuick.Controls 2.13
import QtQuick.Controls.Material 2.3
import QtQuick.Layouts 1.13

WalletDialog {
    title: stack_view.currentItem.title
    width: 320
    height: 400

    Component {
        id: confirm_pin_view
        PinView {
            onPinChanged: {
                if (valid && pin !== pin_view.pin) {
                    clear();
                    ToolTip.show(qsTrId('id_pins_do_not_match_please_try'), 1000);
                }
            }
            property bool accept: valid && pin === pin_view.pin
            property string title: qsTrId('id_verify_your_pin')
        }
    }

    StackView {
        id: stack_view
        anchors.fill: parent
        anchors.margins: 20
        initialItem: PinView {
            id: pin_view
            property bool accept: false
            property string title: qsTrId('id_set_a_new_pin')
            onValidChanged: {
                if (valid) {
                    stack_view.push(confirm_pin_view)
                }
            }
        }
    }

    footer: DialogButtonBox {
        Button {
            enabled: stack_view.currentItem.accept
            flat: true
            text: qsTrId('id_ok')
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        Button {
            flat: true
            text: qsTrId('id_cancel')
            onClicked: close()
        }
    }

    onAccepted: wallet.changePin(pin_view.pin)
    onClosed: destroy()
}

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// A dialog's text button. The primary one is filled with the window's accent
// (Material.accent, which follows the omarchy theme).
Rectangle {
    id: dialogButton
    implicitWidth: dialogButtonLabel.implicitWidth + 28
    implicitHeight: 34
    radius: 8

    property string text: ""
    property bool primary: false
    // Black or white, whichever reads on the accent — as Backend::foregroundFor.
    readonly property color accentForeground:
        0.299 * Material.accent.r + 0.587 * Material.accent.g + 0.114 * Material.accent.b < 0.5
            ? "white" : "black"
    signal clicked()

    color: primary ? Material.accent : "#2c2c2f"
    opacity: enabled ? 1 : 0.45
    border.color: activeFocus ? (primary ? accentForeground : Material.accent) : "transparent"
    border.width: activeFocus ? 2 : 0

    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()
    Keys.onSpacePressed: clicked()

    Label {
        id: dialogButtonLabel
        anchors.centerIn: parent
        text: dialogButton.text
        color: dialogButton.primary ? dialogButton.accentForeground : "white"
        font.pixelSize: 13
        font.weight: Font.DemiBold
    }
    MouseArea {
        anchors.fill: parent
        enabled: dialogButton.enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: dialogButton.clicked()
    }
}

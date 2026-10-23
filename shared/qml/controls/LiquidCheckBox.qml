import QtQuick
import QtQuick.Controls
import "../foundation" as Foundation

CheckBox {
    id: root
    implicitHeight: 34
    spacing: 8
    indicator: LiquidSurface {
        implicitWidth: 16
        implicitHeight: 16
        x: root.leftPadding
        y: (root.height - height) / 2
        radius: 4
        tintColor: root.checked ? Foundation.AppTheme.accent : "#ffffff"
        tintStrength: root.checked ? 0.38 : 0.05
        // §4.6 复选框：选中 accent + 白勾；半选横线；热区 44。
        Text {
            anchors.centerIn: parent
            text: "✓"
            visible: root.checkState === Qt.Checked
            color: "#ffffff"
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }
        Rectangle {
            anchors.centerIn: parent
            width: 8
            height: 2
            radius: 1
            color: "#ffffff"
            visible: root.checkState === Qt.PartiallyChecked
        }
        MouseArea {
            anchors.centerIn: parent
            width: 44
            height: 44
            enabled: root.enabled
            onClicked: root.toggle()
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: 6
            color: "transparent"
            border.width: 2
            border.color: Foundation.AppTheme.focusRing
            visible: root.activeFocus
        }
    }
    contentItem: Label {
        text: root.text
        font: root.font
        leftPadding: root.indicator.width + root.spacing
        verticalAlignment: Text.AlignVCenter
        color: Foundation.AppTheme.text
    }
    opacity: enabled ? 1 : 0.38
}

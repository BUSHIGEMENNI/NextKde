import QtQuick
import QtQuick.Window
import Kos.Ui

// Calendar-owned placement and state; optics remain in the shared Demo component.
Item {
    id: root
    property LiquidBackdrop backdrop: null
    property real radius: height / 2
    property bool hovered: false
    property bool pressed: false
    property bool focused: false
    property bool liquidEnabled: AppTheme.materialMode !== "solid" && !AppTheme.reduceTransparency
    property real coverage: 0.18
    // The shared preference scales each role's coverage around its authored
    // 0.86 default, keeping controls lighter than floating panels.
    readonly property real effectiveCoverage: Math.min(0.98, coverage * AppTheme.materialOpacity / 0.86)
    property real refractionStrength: 1.5
    property real magnification: 1.012
    property real reflectionStrength: hovered || focused ? 0.36 : 0.28
    property color fallbackColor: design.controlFill
    readonly property bool sampling: liquidEnabled && glass.sampling
    CalendarStyle { id: design }

    LiquidCompactSurface {
        id: glass
        anchors.fill: parent
        radius: root.radius
        backdrop: root.liquidEnabled ? root.backdrop : null
        visible: root.sampling
        convexLens: true
        fullHighlight: true
        tintColor: AppTheme.dark ? "#1F1F22" : "#FFFFFF"
        tintStrength: Math.min(1, root.effectiveCoverage + (root.pressed ? 0.10 : root.hovered ? 0.04 : 0))
        Behavior on tintStrength { NumberAnimation { duration: AppTheme.motionFast } }
        refractionStrength: root.refractionStrength
        magnification: root.magnification
        reflectionStrength: root.reflectionStrength
        blurStrength: 0.45
        readability: 0
        edgeWidth: Math.min(12, height / 2)
        rimColor: AppTheme.dark ? Qt.rgba(1, 1, 1, 0.74) : Qt.rgba(0.2, 0.2, 0.22, 0.26)
    }
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        visible: !root.sampling
        color: root.pressed ? design.selectionFill : root.hovered ? design.rowHoverFill : root.fallbackColor
        border.width: root.focused ? 2 : 0
        border.color: AppTheme.focusRing
    }
}

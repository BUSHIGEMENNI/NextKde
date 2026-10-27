pragma ComponentBehavior: Bound

import QtQuick
import Kos.Ui

// Calendar placement and selection state; CalendarGlassSurface supplies the
// shared Demo optics. Both surfaces sample chromePlanes, excluding this control.
FocusScope {
    id: root

    implicitWidth: 180
    implicitHeight: 34

    property var labels: []
    property int currentIndex: 0
    property color backgroundColor: AppTheme.button
    property color selectionColor: AppTheme.cardHover
    property color textColor: AppTheme.text
    property color mutedTextColor: AppTheme.mutedText
    property LiquidBackdrop backdrop: null
    readonly property bool sampling: track.sampling && lens.sampling
    opacity: enabled ? 1 : 0.5
    HoverHandler { id: hover }
    property int _pressedIndex: -1
    property int _visualIndex: 0
    property int _pendingIndex: -1
    property bool _ready: false
    readonly property bool lensExpanded: sampling && !AppTheme.reduceMotion
        && (_pressedIndex >= 0 || releasePulse.running)

    function resetPress() {
        _pressedIndex = -1
        releasePulse.stop()
    }
    onEnabledChanged: { if (!enabled) resetPress() }
    onVisibleChanged: { if (!visible) resetPress() }
    on_VisualIndexChanged: {
        if (_ready && !AppTheme.reduceMotion)
            releasePulse.restart()
    }

    readonly property int safeIndex: clampedIndex(currentIndex)
    readonly property real segmentWidth: labels.length > 0 ? (width - 4) / labels.length : 0

    signal selectionRequested(int index)

    function clampedIndex(index) {
        return labels.length > 0
            ? Math.max(0, Math.min(labels.length - 1, index)) : 0
    }

    function select(index) {
        if (!enabled || index < 0 || index >= labels.length)
            return
        // Draw the result first. The owner may update its persistent state
        // through IPC, but that round trip must not consume the first click.
        if (index === _visualIndex && !AppTheme.reduceMotion)
            releasePulse.restart()
        _pendingIndex = index
        _visualIndex = index
        selectionRequested(index)
        confirmationTimer.restart()
    }

    function selectAndFocus(index) {
        const target = Math.max(0, Math.min(labels.length - 1, index))
        select(target)
        const segment = segments.itemAt(target)
        if (segment)
            segment.forceActiveFocus()
    }

    onCurrentIndexChanged: {
        // Read the changed source property directly. A binding that depends on
        // currentIndex (such as safeIndex) may not have been re-evaluated yet
        // when this change handler runs.
        _visualIndex = clampedIndex(currentIndex)
        _pendingIndex = -1
        confirmationTimer.stop()
    }
    onLabelsChanged: {
        _visualIndex = clampedIndex(currentIndex)
        _pendingIndex = -1
    }
    Component.onCompleted: {
        _visualIndex = safeIndex
        _ready = true
    }

    // Keep very short clicks and keyboard changes visible long enough to swell,
    // then let the lens settle. A new selection restarts this pulse in place.
    Timer {
        id: releasePulse
        interval: 140
    }

    Timer {
        id: confirmationTimer
        interval: Math.max(420, AppTheme.motionNormal * 3)
        repeat: false
        // Roll the lens back only if the externally-owned state did not
        // acknowledge this optimistic selection.
        onTriggered: {
            root._visualIndex = root.safeIndex
            root._pendingIndex = -1
        }
    }

    CalendarGlassSurface {
        id: track
        objectName: "calendarViewTrack"
        anchors.fill: parent
        backdrop: root.backdrop
        radius: height / 2
        coverage: 0.18
        hovered: hover.hovered
        fallbackColor: root.backgroundColor
    }

    CalendarGlassSurface {
        id: lens
        objectName: "calendarViewLens"
        x: 4 + root._visualIndex * root.segmentWidth
        y: 2
        width: root.segmentWidth - 4
        height: root.height - 4
        radius: height / 2
        backdrop: root.backdrop
        // Demo segmented lens: 1.12 press expansion with an OutBack return.
        // Only the sampled lens deforms; labels and hit targets stay stationary.
        scale: root.lensExpanded ? 1.12 : 1
        Behavior on scale {
            NumberAnimation {
                duration: AppTheme.reduceMotion ? 0 : AppTheme.motionNormal
                easing.type: Easing.OutBack
                easing.overshoot: 1.4
            }
        }
        coverage: root.lensExpanded ? 0.06 : 0.80
        refractionStrength: root.lensExpanded ? 4.5 : 3
        magnification: root.lensExpanded ? 1.045 : 1.025
        reflectionStrength: root.lensExpanded || hovered || focused ? 0.72 : 0.50
        Behavior on coverage { NumberAnimation { duration: AppTheme.motionFast } }
        Behavior on refractionStrength { NumberAnimation { duration: AppTheme.motionFast } }
        Behavior on magnification { NumberAnimation { duration: AppTheme.motionFast } }
        Behavior on reflectionStrength { NumberAnimation { duration: AppTheme.motionFast } }
        fallbackColor: root.selectionColor
        hovered: hover.hovered
        pressed: root._pressedIndex >= 0
        // Focus changes the neutral glass reflection, never an accent overlay.
        focused: root.activeFocus
        Behavior on x {
            NumberAnimation {
                duration: AppTheme.reduceMotion ? 0 : AppTheme.motionNormal
                easing.type: Easing.OutQuint
            }
        }
    }

    Repeater {
        id: segments
        model: root.labels
        delegate: Item {
            id: segmentDelegate

            required property var modelData
            required property int index
            x: 2 + segmentDelegate.index * root.segmentWidth
            width: root.segmentWidth
            height: root.height
            enabled: root.enabled
            activeFocusOnTab: enabled

            Accessible.role: Accessible.RadioButton
            Accessible.name: String(segmentDelegate.modelData)
            Accessible.checked: segmentDelegate.index === root._visualIndex
            Accessible.focusable: enabled
            Accessible.focused: activeFocus
            Accessible.onPressAction: root.select(segmentDelegate.index)

            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Left) {
                    root.selectAndFocus(segmentDelegate.index - 1)
                } else if (event.key === Qt.Key_Right) {
                    root.selectAndFocus(segmentDelegate.index + 1)
                } else if (event.key === Qt.Key_Home) {
                    root.selectAndFocus(0)
                } else if (event.key === Qt.Key_End) {
                    root.selectAndFocus(root.labels.length - 1)
                } else if (event.key === Qt.Key_Space
                           || event.key === Qt.Key_Enter
                           || event.key === Qt.Key_Return) {
                    root.select(segmentDelegate.index)
                } else {
                    return
                }
                event.accepted = true
            }

            Text {
                anchors.centerIn: parent
                width: Math.max(0, parent.width - 16)
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: segmentDelegate.modelData
                color: segmentDelegate.index === root._visualIndex
                    ? root.textColor : root.mutedTextColor
                font.pixelSize: 12
                font.weight: segmentDelegate.index === root._visualIndex
                    ? Font.DemiBold : Font.Normal
                scale: root._pressedIndex === segmentDelegate.index ? 0.96 : 1.0
                Behavior on color {
                    ColorAnimation { duration: AppTheme.reduceMotion ? 0 : AppTheme.motionFast }
                }
                Behavior on scale {
                    NumberAnimation {
                        duration: AppTheme.reduceMotion ? 0 : AppTheme.motionFast
                        easing.type: Easing.OutCubic
                    }
                }
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onPressed: {
                    segmentDelegate.forceActiveFocus()
                    root._pressedIndex = segmentDelegate.index
                }
                onCanceled: root.resetPress()
                onReleased: root._pressedIndex = -1
                onClicked: root.select(segmentDelegate.index)
            }
        }
    }
}

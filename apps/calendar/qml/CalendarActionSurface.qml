import QtQuick
import Kos.Ui

// Only the optical background deforms; glyphs and hit targets stay fixed.
CalendarGlassSurface {
    id: root
    readonly property bool expanded: sampling && !AppTheme.reduceMotion
        && (pressed || releasePulse.running)
    onPressedChanged: {
        if (pressed && !AppTheme.reduceMotion) releasePulse.restart()
    }
    Timer { id: releasePulse; interval: 140 }
    scale: !sampling || AppTheme.reduceMotion ? 1 : expanded ? 1.10 : hovered ? 1.025 : 1
    refractionStrength: expanded ? 4.5 : hovered ? 3 : 1.5
    magnification: expanded ? 1.045 : hovered ? 1.025 : 1.012
    reflectionStrength: expanded ? 0.65 : hovered || focused ? 0.42 : 0.28
    Behavior on scale {
        NumberAnimation { duration: AppTheme.motionNormal; easing.type: Easing.OutBack }
    }
    Behavior on refractionStrength { NumberAnimation { duration: AppTheme.motionFast } }
    Behavior on magnification { NumberAnimation { duration: AppTheme.motionFast } }
    Behavior on reflectionStrength { NumberAnimation { duration: AppTheme.motionFast } }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import "../../../shared/qml/controls" as LiquidControls
import "../../../shared/qml/foundation" as Foundation
import "../../../shared/qml/i18n"

RowLayout {
    required property var colors
    id: sliderRow
    property string label: ""
    property string unit: ""
    property real minV: 0
    property real maxV: 100
    property real current: 0
    // 小数位（0 = 整数步进与显示，现状；>0 用于倍率类参数如悬停放大）
    property int decimals: 0
    property bool active: true
    property var onCommit: null
    property real _preview: -1
    readonly property real _shown: _preview >= 0 ? _preview : current

    Layout.fillWidth: true
    spacing: 8

    Text {
        text: sliderRow.label
        color: sliderRow.active ? colors.primaryText : colors.secondaryText
        font.pixelSize: 13
        Layout.preferredWidth: 148
    }
    Text {
        text: sliderRow.decimals > 0
            ? sliderRow._shown.toFixed(sliderRow.decimals) + sliderRow.unit
            : Math.round(sliderRow._shown) + sliderRow.unit
        color: colors.secondaryText
        font.pixelSize: 12
        Layout.preferredWidth: 46
        horizontalAlignment: Text.AlignRight
    }
    LiquidControls.LiquidSlider {
        objectName: "stage-value-slider"
        accentColor: colors.accent
        trackColor: colors.divider
        Layout.fillWidth: true
        opacity: sliderRow.active ? 1.0 : 0.4
        enabled: sliderRow.active
        // Ctrl+滚轮一档 = 一个能显示出来的单位（整数就 1，小数按 decimals），
        // 换成归一化行程。倍率这类窄区间（0.9~1.1）才不会每档都被舍掉。
        wheelStep: {
            const span = sliderRow.maxV - sliderRow.minV
            if (span <= 0)
                return 0.01
            const unit = sliderRow.decimals > 0
                ? Math.pow(10, -sliderRow.decimals) : 1
            return Math.min(1, unit / span)
        }
        value: sliderRow.maxV > sliderRow.minV
            ? (sliderRow._shown - sliderRow.minV)
              / (sliderRow.maxV - sliderRow.minV) : 0
        onPreviewChanged: function(position) {
            sliderRow._preview = sliderRow.minV
                + position * (sliderRow.maxV - sliderRow.minV)
        }
        onCommitRequested: function(position) {
            sliderRow._preview = -1
            if (sliderRow.onCommit) {
                const v = sliderRow.minV
                    + position * (sliderRow.maxV - sliderRow.minV)
                sliderRow.onCommit(sliderRow.decimals > 0
                    ? Number(v.toFixed(sliderRow.decimals))
                    : Math.round(v))
            }
        }
    }
}

/*
    SPDX-FileCopyrightText: 2008 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2026 fg-sched stage animation

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effectwindow.h"
#include "effect/offscreeneffect.h"
#include "effect/timeline.h"
#include "opengl/glframebuffer.h"
#include "opengl/glshader.h"
#include "opengl/gltexture.h"

#include <QFileSystemWatcher>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QVector>

namespace KWin
{

struct StageTarget; // 定义在 stageanim.cpp（shell 发布的卡片矩形条目）

struct StageAnimAnimation
{
    EffectWindowVisibleRef visibleRef;
    TimeLine timeLine;
    // 每窗目标（shell 按 KWin internalId 发布的卡片矩形）；无效 = 全局/回落
    QRect target;
    qreal endScale = -1.0;
    bool flat = false; // 目标卡片是正视的（中心拖放）：无倾斜分量
};

// 活体卡姿态：终态卡面（plate）未倾斜矩形 + 透视参数。与 QML 的
// stage_tilt.frag 同一套数学（tiltProject 孪生），角/yOff/焦距全部由
// shell 按当前状态发布（悬停压平/列滚动都会改变它们）。
struct LiveCardPose
{
    QRectF rect;      // 屏幕逻辑坐标的卡面矩形（= 快照所在 plate）
    qreal angleDeg = 0; // 已含右侧镜像符号的终态倾角
    qreal yOff = 0;   // 卡中心相对共享地平线的 y 偏移（正 = 地平线下方）
    qreal focal = 2200;
    qreal radius = 10; // 圆角半径（与 plate.radius 同源）
};

// 一张活体卡：持 refOffscreenRendering（KWin 给隐藏窗补发帧回调 →
// 客户端继续出帧，成本等同窗口可见）+ 窗口内容渲染进卡面尺寸的
// 小 FBO（损伤驱动重拍，填充率可忽略），绘制时逐像素逆投影 + 圆角
// SDF，画进侧栏浮层窗口的绘制过程中（chrome 之下、桌面之上）。
struct LiveCard
{
    QString id;
    QPointer<EffectWindow> window;
    LiveCardPose target;  // 最新发布的终态
    LiveCardPose from;    // 缓动起点
    TimeLine ease{std::chrono::milliseconds(220)};
    bool easing = false;
    std::unique_ptr<EffectWindowVisibleRef> visibleRef;
    bool offscreenRef = false;
    QMetaObject::Connection damageConnection;
    std::unique_ptr<GLTexture> texture; // 卡面尺寸 × dpr 的小纹理
    std::unique_ptr<GLFramebuffer> fbo;
    bool dirty = true; // 窗口有新损伤待重拍
    qreal dpr = 1.0;  // 最近一次绘制时的输出缩放（纹理分配依据）
    // 诊断计数（LiveTrace 节流日志）
    quint32 damageCount = 0;
    quint32 renderCount = 0;
    quint32 paintCount = 0;

    // ── 卡面本体（方案"卡进特效"：chrome 由特效同管线绘制）──
    // 元数据（QML 发布，v2 协议）
    QString title;        // 显示名（QML 拼好含 ×N 后缀）
    int count = 1;        // 组内窗数（扇叠背板数 = min(count-1, 2)）
    qreal z = 0;          // 叠序（QML slot.z）
    bool dragging = false; // 跟手模式：矩形逐帧由 QML 发布，特效免悬停
    bool engaging = false; // 展开中：整体淡出后让位给窗口动画
    bool dropHover = false; // 拖放目标预示（边框蓝）
    bool dwellHint = false; // 驻留合并预示（边框蓝）
    QColor tint{13, 18, 31};      // 背板基色（QML rgba(0.05,0.07,0.12,tint)）
    QColor tintHover{26, 33, 51}; // 悬停提亮（×1.3 色族）
    QColor border{255, 255, 255, 71};
    qreal hoverScale = 1.18;
    qreal hoverTiltDeg = 0;       // 悬停终态倾角（已含右条镜像符号）
    std::chrono::milliseconds hoverMs{240};
    qreal fanSpacing = 6;
    qreal depthStrength = 0.22;
    qreal topLight = 0.10;
    // 悬停状态机（特效自驱：cursorPos 命中静止矩形——与 QML 输入区同界；
    // 动画在本进程跑，与内容同管线同时钟 = 像素级同步）
    bool hovered = false;
    qreal curScale = 1.0;   // 当前插值（绘制用）
    qreal curTiltDeg = 0;
    qreal scaleFrom = 1.0, scaleTo = 1.0, tiltFrom = 0, tiltTo = 0;
    TimeLine hoverTl{std::chrono::milliseconds(240)};
    bool hoverAnimating = false;
    // 入场/退场/engaging 淡变（alphaFrom→alphaTo）× QML 发布的 fade
    //（压暗 × 视口边缘渐隐）
    qreal alpha = 1.0;
    qreal alphaFrom = 1.0, alphaTo = 1.0;
    qreal fade = 1.0;
    bool closeHot = false;
    TimeLine fadeTl{std::chrono::milliseconds(180)};
    bool fadeAnimating = false;
    // 铭牌（标题/关闭钮，QPainter 光栅 → 纹理；键变才重绘，Y 镜像匹配
    // stage-live 的 FBO 朝向采样）
    std::unique_ptr<GLTexture> chromeTex;
    QString chromeKey;
};

// MagicLamp derivative whose minimize target is resolved per animation
// trigger: ① the card rect the shell publishes for this window (KWin
// internalId) — the window shrinks into / grows out of its own Stage sidebar
// card; ② the global strip rect from kwinrc [Effect-stageanim] Target*;
// ③ the original MagicLamp behaviour (iconGeometry / cursor fallback).
class StageAnimEffect : public OffscreenEffect
{
    Q_OBJECT

public:
    StageAnimEffect();

    void reconfigure(ReconfigureFlags) override;
    void prePaintScreen(ScreenPrePaintData &data, std::chrono::milliseconds presentTime) override;
    void paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport,
                     int mask, const Region &deviceRegion, LogicalOutput *screen) override;
    void prePaintWindow(RenderView *view, EffectWindow *w, WindowPrePaintData &data, std::chrono::milliseconds presentTime) override;
    void postPaintScreen() override;
    bool isActive() const override;

    int requestedEffectChainPosition() const override
    {
        return 50;
    }

    static bool supported();

protected:
    void apply(EffectWindow *window, int mask, WindowPaintData &data, WindowQuadList &quads) override;

public Q_SLOTS:
    void slotWindowAdded(KWin::EffectWindow *w);
    void slotWindowDeleted(KWin::EffectWindow *w);
    void slotWindowMinimized(KWin::EffectWindow *w);
    void slotWindowUnminimized(KWin::EffectWindow *w);

private:
    void resolveTarget(KWin::EffectWindow *w, StageAnimAnimation &anim, const QVector<StageTarget> &targets);
    std::chrono::milliseconds m_duration;
    QHash<EffectWindow *, StageAnimAnimation> m_animations;
    QSet<EffectWindow *> m_connected; // 已挂 minimizedChanged 的窗口
    QRect m_target; // 全局配置目标（Stage 侧栏区域）；无效 = 回落行为
    QEasingCurve m_easing{QEasingCurve::InOutCubic};
    qreal m_tiltAngle = 22.0; // kwinrc TiltAngle：卡片倾斜角，动画起止姿态
    qreal m_glassOpacity = 0.65; // kwinrc GlassOpacity：飞行途中透明度（1=关）
    bool m_trace = false; // kwinrc TraceTargets：正常路径也打目标解析日志
    bool m_mirrorTargets = false; // kwinrc TargetMirror：右侧常驻——装卡姿态镜像

    // ── 活体卡（合成器直绘，stage-live.json 由 shell 发布）──
    void reloadLiveCards();
    void detachLiveCard(LiveCard &card);
    void releaseLiveCard(LiveCard &card);
    void renderLiveTexture(LiveCard &card);
    void drawLiveCards(const RenderTarget &renderTarget, const RenderViewport &viewport);
    void writeLiveStatus();
    bool liveCardPaintable(const LiveCard &card) const;
    static LiveCardPose lerpPose(const LiveCardPose &a, const LiveCardPose &b, qreal t);
    static LiveCardPose currentPose(const LiveCard &card);
    // 卡面铭牌（标题/关闭钮）光栅化；键（标题/计数/尺寸）变才重绘
    void rasterChrome(LiveCard &card);

    QString m_livePath;
    QString m_liveStatusPath;
    QFileSystemWatcher *m_liveWatcher = nullptr;
    QTimer m_liveStatusTimer;   // 周期刷 status 文件（shell 据此让位快照）
    QTimer m_liveStaleTimer;    // 心跳超时 → 撤引用（shell 死亡防挂死）
    QTimer m_liveFrameTimer;    // 自驱帧回调投喂（30Hz framePainted）
    QHash<QString, QSharedPointer<LiveCard>> m_liveCards;
    QHash<QString, QSharedPointer<LiveCard>> m_liveFading; // 退场淡出 ghost
    QSet<QString> m_livePending; // 文件里有、窗口还没出现（等 windowAdded）
    std::unique_ptr<GLShader> m_liveShader;
    std::unique_ptr<GLShader> m_cardShader; // 卡面整体（背板/渐变/边框/内容合一）
    bool m_liveEnabled = true; // kwinrc LiveCards 总闸（默认开，排障用）
};

} // namespace

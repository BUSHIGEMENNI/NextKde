/*
    KWin - the KDE window manager. Minimize/restore animation for Stage 侧栏.
    Uniform scale + translate + end fade: the window shrinks into its own
    sidebar card and grows back out of it (macOS Stage Manager style — no
    content warping, aspect-preserving).

    Derived from KWin MagicLamp (Martin Gräßlin, GPL-2.0-or-later).

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "stageanim.h"
#include "effect/effecthandler.h"
#include "opengl/glutils.h"
#include "compositor.h"
#include "scene/item.h"
#include "scene/itemrenderer.h"
#include "scene/scene.h"
#include "scene/workspacescene.h"
#include "scene/windowitem.h"
#include "window.h"
#include "opengl/glvertexbuffer.h"

#include <QDateTime>
#include <QImage>
#include <QPainter>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <cmath>

using namespace std::chrono_literals;

namespace KWin
{

// Categorized logging so target resolution is verifiable from the journal
// (uncategorized qWarnings from KWin plugins never reach it).
Q_LOGGING_CATEGORY(STAGEANIM_LOG, "kwin.effects.stageanim", QtInfoMsg)

// 目标解析日志：card / card-flat 是正常路径（后者 = 中心拖放的直长动画），
// 仅在 kwinrc [Effect-stageanim] TraceTargets=true 时输出（调参/验证用，
// reconfigureEffect 即时生效）；global/fallback 是异常路径（没查到卡片
// 矩形），始终 warning 提醒。
static void logTarget(bool trace, const char *source, const QString &id,
                      const QRect &rect, qreal scale)
{
    if (trace || (qstrcmp(source, "card") != 0
                  && qstrcmp(source, "card-flat") != 0))
        qCWarning(STAGEANIM_LOG) << "target=" << source << "id=" << id
                                 << "rect=" << rect << "scale=" << scale;
}

// ── 调参常量（与 shell/desktop/modules/stage/stage-geometry.mjs 对应项
// 保持一致——改一处必须同步另一处）──
static constexpr qreal kPerspectiveFocal = 900.0;   // 针孔透视焦距
static constexpr qreal kFadeTailFraction = 0.3;     // 末段淡出占比
static constexpr qreal kCardMinEndScale = 0.08;     // 装卡缩放下限
static constexpr qreal kCardMaxEndScale = 0.5;      // 装卡缩放上限
static constexpr qreal kGlobalMinScale = 0.10;      // 全局矩形缩放下限
static constexpr qreal kGlobalMaxScale = 0.40;      // 全局矩形缩放上限
static constexpr qreal kDefaultGlassOpacity = 0.65; // 飞行玻璃透明度默认

// shell（StageSidebarWindow.publishTargets）按窗口 KWin internalId 发布的
// 卡片矩形。动画起止点 = 那扇窗口自己的卡片位置与尺寸（macOS 式连续交换）。
struct StageTarget
{
    QString id; // KWin internalId（无花括号），与 shell windowId 逐字一致
    QRect rect;
    bool flat = false; // shell 标记：从"正视卡片"直长/直收（中心拖放），
                       // 动画不带倾斜分量
};

// 目标文件路径：$XDG_STATE_HOME/quickshell/kos/fg-sched/stage-targets.json
//（由 shell 的 JsonConfigStore → platform state.write 原子写入）
static QString stageTargetsPath()
{
    const QString stateHome = qEnvironmentVariable("XDG_STATE_HOME",
        QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
            + QStringLiteral("/.local/state"));
    return stateHome + QStringLiteral(
        "/quickshell/kos/fg-sched/stage-targets.json");
}

// 活体卡描述文件（shell 的 StageSidebarWindow.publishLiveCards 原子写入）：
//   { "at": <ms>, "cards": [ { id, x, y, w, h, angle, yOff, focal, radius } ] }
// id = 卡片代表窗的 KWin internalId（无花括号）；x/y/w/h = 终态卡面
//（plate，含悬停缩放）未倾斜屏幕矩形；angle 已含右侧镜像符号。
static QString stageLivePath()
{
    const QString stateHome = qEnvironmentVariable("XDG_STATE_HOME",
        QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
            + QStringLiteral("/.local/state"));
    return stateHome + QStringLiteral(
        "/quickshell/kos/fg-sched/stage-live.json");
}

// 特效回执（shell 据此把卡片快照让位给活体绘制；不新鲜则回退快照）：
//   { "at": <ms>, "active": bool, "cards": [id...] }
static QString stageLiveStatusPath()
{
    return stageLivePath() + QStringLiteral(".status");
}

// targets 文件的完整快照：卡片矩形 + suppress 名单。每次动画触发只读
// 一次（旧实现 isSuppressed 与 resolveTarget 各开一遍文件——两次解析
// 之间 shell 改写会产生撕裂读，且白白翻倍文件 IO）
struct StageTargetsFile
{
    QVector<StageTarget> targets;
    QSet<QString> suppress;
};

static StageTargetsFile loadStageTargets()
{
    StageTargetsFile out;
    QFile f(stageTargetsPath());
    if (!f.open(QIODevice::ReadOnly))
        return out;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (doc.isNull()) {
        // 坏 JSON（半截写入/磁盘问题）与"shell 未运行（文件不存在）"要
        // 区分得开：前者是故障信号，静默回落会掩盖排障线索。只告警一次
        // ——本函数逐事件调用，坏文件常驻时不能刷屏
        static bool warnedBadJson = false;
        if (!warnedBadJson) {
            warnedBadJson = true;
            qCWarning(STAGEANIM_LOG,
                "stage-targets.json exists but is not valid JSON");
        }
        return out;
    }
    const auto obj = doc.object();
    const auto arr = obj.value(QStringLiteral("targets")).toArray();
    out.targets.reserve(arr.size());
    for (const auto &v : arr) {
        const auto o = v.toObject();
        const QRect r(o.value(QStringLiteral("x")).toInt(),
                      o.value(QStringLiteral("y")).toInt(),
                      o.value(QStringLiteral("width")).toInt(),
                      o.value(QStringLiteral("height")).toInt());
        if (!r.isValid())
            continue;
        StageTarget t;
        t.id = o.value(QStringLiteral("id")).toString();
        t.rect = r;
        t.flat = o.value(QStringLiteral("flat")).toInt(0) == 1;
        out.targets.append(t);
    }
    // suppress 名单内的窗口最小化/还原**跳过动画**（瞬间完成）——实时
    // 卡片模式的静默收放全靠它（悬停预备静默最小化、实时恢复静默还原
    // +keepBelow 压底都不能打扰视觉）。恒空：伪实时已删，保留解析仅为
    // 文件格式兼容
    const auto sup = obj.value(QStringLiteral("suppress")).toArray();
    for (const auto &v : sup)
        out.suppress.insert(v.toString());
    return out;
}

// suppress 名单命中判定（名单语义见 loadStageTargets 内注释）
static bool isSuppressed(const QSet<QString> &suppress, const EffectWindow *w)
{
    return suppress.contains(w->internalId().toString(QUuid::WithoutBraces));
}

StageAnimEffect::StageAnimEffect()
{
    reconfigure(ReconfigureAll);
    connect(effects, &EffectsHandler::windowAdded, this, &StageAnimEffect::slotWindowAdded);
    connect(effects, &EffectsHandler::windowDeleted, this, &StageAnimEffect::slotWindowDeleted);

    // windowAdded 只对"加载之后"新建的窗口发——dbus loadEffect 落在会话
    // 中段，开机就存在的窗口不会补发信号，连接必须自己对 stackingOrder
    // 全量补建（否则那批窗口永远没有最小化/还原动画）。QSet 防重连。
    const QList<EffectWindow *> windows = effects->stackingOrder();
    for (EffectWindow *w : windows)
        slotWindowAdded(w);

    // ── 活体卡输入通道：目录 + 文件双 watch（JsonConfigStore 原子重写
    // 会换 inode 拆掉文件 watch，目录 watch 兜住创建/替换）。30ms 去抖
    // 折叠 shell 的发布连发。10s 周期 reload 兜 missed events + 文件里
    // 有但窗口还没出现的 pending 重试（windowAdded 也触发）。
    m_livePath = stageLivePath();
    m_liveStatusPath = stageLiveStatusPath();
    m_liveWatcher = new QFileSystemWatcher(this);
    const QString liveDir = QFileInfo(m_livePath).absolutePath();
    m_liveWatcher->addPath(liveDir);
    m_liveWatcher->addPath(m_livePath);
    connect(m_liveWatcher, &QFileSystemWatcher::fileChanged, this, [this]() {
        if (!m_liveWatcher->files().contains(m_livePath))
            m_liveWatcher->addPath(m_livePath);
        QTimer::singleShot(16, this, &StageAnimEffect::reloadLiveCards);
    });
    connect(m_liveWatcher, &QFileSystemWatcher::directoryChanged, this, [this]() {
        if (!m_liveWatcher->files().contains(m_livePath))
            m_liveWatcher->addPath(m_livePath);
        QTimer::singleShot(16, this, &StageAnimEffect::reloadLiveCards);
    });
    m_liveStaleTimer.setInterval(10000);
    connect(&m_liveStaleTimer, &QTimer::timeout, this, &StageAnimEffect::reloadLiveCards);
    m_liveStaleTimer.start();
    m_liveStatusTimer.setInterval(8000);
    connect(&m_liveStatusTimer, &QTimer::timeout, this, [this]() {
        if (m_liveCards.isEmpty())
            return;
        for (auto it = m_liveCards.begin(); it != m_liveCards.end(); ++it) {
            LiveCard &card = **it;
            if (card.dirty && liveCardPaintable(card))
                renderLiveTexture(card);
        }
        writeLiveStatus();
    });
    m_liveStatusTimer.start();
    // 自驱帧回调：KWin 内部 offscreen 计时器路径实测未给最小化窗送回调
    //（原因未明），直接周期调用公开的 WindowItem::framePainted——与
    // Window::maybeSendFrameCallback 同款调用。客户端有待决帧请求时被
    // 喂到 → 渲染 → 提交 → Window::damaged → dirty + addRepaint。
    m_liveFrameTimer.setInterval(33);
    connect(&m_liveFrameTimer, &QTimer::timeout, this, [this]() {
        // 帧投喂分级：悬停/拖拽/悬停动画中的卡每拍喂（30fps），其余卡
        // 轮流喂（~7.5fps）——每次投喂都会引发客户端渲染→损伤→重拍（整
        // 窗场景树渲进 FBO），全员 30fps 是帧率杀手；侧栏小卡 7.5fps 的
        // 活体感无肉眼差异
        ++m_liveFeedTick;
        for (auto it = m_liveCards.begin(); it != m_liveCards.end(); ++it) {
            LiveCard &card = **it;
            const bool priority = card.hovered || card.dragging
                || card.hoverAnimating || card.engaging;
            if (!priority && (m_liveFeedTick + card.feedPhase) % 4 != 0)
                continue;
            if (!liveCardPaintable(card) || !card.window->windowItem())
                continue;
            const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch());
            card.window->windowItem()->framePainted(nullptr,
                card.window->window() ? card.window->window()->output() : nullptr,
                nullptr, timestamp);
        }
    });
    m_liveFrameTimer.start();
    reloadLiveCards();

    setVertexSnappingMode(RenderGeometry::VertexSnappingMode::None);
}

bool StageAnimEffect::supported()
{
    return OffscreenEffect::supported() && effects->animationsSupported();
}

void StageAnimEffect::reconfigure(ReconfigureFlags)
{
    const KConfigGroup grp = effects->config()->group(QStringLiteral("Effect-stageanim"));
    const std::chrono::milliseconds d(grp.readEntry<int>("AnimationDuration", 420));
    m_duration = std::chrono::milliseconds(static_cast<int>(animationTime(d)));

    const int x = grp.readEntry<int>("TargetX", -1);
    const int y = grp.readEntry<int>("TargetY", -1);
    const int w = grp.readEntry<int>("TargetWidth", -1);
    const int h = grp.readEntry<int>("TargetHeight", -1);
    m_target = (x >= 0 && y >= 0 && w > 0 && h > 0)
        ? QRect(x, y, w, h)
        : QRect();

    // 倾角钳位（对比下方 GlassOpacity）：透视深度 depth = (pivotX − sx)
    // ·sinR 随角变大，k = focal / (focal − depth)——大屏宽 × 45° 时 depth
    // ≈900 恰好触焦，>45° 远缘 focal−depth 变负、k 翻负号，顶点镜像翻转
    // 窗口绘制炸裂。40° 封顶时 1272px 半宽 depth≈818 < focal(900) 恒安全。
    m_tiltAngle = std::clamp(grp.readEntry<double>("TiltAngle", 22.0),
                             0.0, 40.0);

    // 飞行玻璃透明度：1.0 = 关闭（全程不透明），越低越"玻璃"
    //（飞行途中透见桌面，落地/装卡端由 fade 尾巴收掉）
    m_glassOpacity = std::clamp(grp.readEntry<double>("GlassOpacity",
                                                      kDefaultGlassOpacity),
                                0.10, 1.0);

    m_trace = grp.readEntry<bool>("TraceTargets", false);

    // 右侧常驻（shell 的 StageModeService 投影）：装卡姿态镜像——卡片
    // 倾斜在右侧取反（右缘近大），动画终点姿态须与卡片一致
    m_mirrorTargets = grp.readEntry<bool>("TargetMirror", false);

    // 活体卡总闸（默认开——shell 侧 thumbLiveEffect 才是用户开关，
    // 这里只是排障时的硬断路器）
    m_liveEnabled = grp.readEntry<bool>("LiveCards", true);
    if (!m_liveEnabled && !m_liveCards.isEmpty()) {
        m_liveCards.clear();
        writeLiveStatus();
    }

    // 缓动曲线可配（kwinrc EasingCurve，默认 OutCubic 无阻尼）
    const QString curve = grp.readEntry<QString>("EasingCurve", QStringLiteral("OutCubic"));
    if (curve == QLatin1String("InOutCubic"))
        m_easing.setType(QEasingCurve::InOutCubic);
    else if (curve == QLatin1String("OutBack"))
        m_easing.setType(QEasingCurve::OutBack);
    else if (curve == QLatin1String("OutQuad"))
        m_easing.setType(QEasingCurve::OutQuad);
    else if (curve == QLatin1String("InOutQuad"))
        m_easing.setType(QEasingCurve::InOutQuad);
    else if (curve == QLatin1String("Linear"))
        m_easing.setType(QEasingCurve::Linear);
    else
        m_easing.setType(QEasingCurve::OutCubic);
}

void StageAnimEffect::prePaintScreen(ScreenPrePaintData &data, std::chrono::milliseconds presentTime)
{
    // Mark the screen as transformed so the moving window is repainted fully.
    // 活体卡期间不动这个 mask（自有分区 addRepaint，不需要全屏重绘）。
    if (!m_animations.isEmpty())
        data.mask |= PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS;

    // 活体卡缓动推进（发布姿态变化 → 短缓动追上，抹平取整抖动）：
    // 未完 → 持续请求卡区重绘
    for (auto it = m_liveCards.begin(); it != m_liveCards.end(); ++it) {
        LiveCard &card = **it;
        if (!card.easing)
            continue;
        card.ease.advance(presentTime);
        if (card.ease.done())
            card.easing = false;
        const QRectF r = currentPose(card).rect;
        effects->addRepaint(r.adjusted(-40, -40, 40, 40).toAlignedRect());
    }

    // ── 悬停引擎（特效自驱）── 卡面视觉已整体由特效绘制（方案"卡进
    // 特效"），悬停放大/压平/展开淡出动画在本进程跑——与内容同管线、
    // 同一时钟，像素级同步（跨文件追赶的脱节就此根治）。命中判定 =
    // 静止矩形（与 QML 输入区同界）+ 悬停中放宽到放大矩形（QML 缩放
    // MouseArea 的语义等价，防边缘闪烁）。曲线对齐 QML：scale OutBack
    // overshoot 1.2、tilt OutCubic。
    if (!m_liveCards.isEmpty()) {
        const QPointF cur = effects->cursorPos();
        QVector<LiveCard *> order;
        order.reserve(m_liveCards.size());
        for (auto it = m_liveCards.begin(); it != m_liveCards.end(); ++it)
            order.append(it.value().data());
        std::sort(order.begin(), order.end(),
                  [](const LiveCard *a, const LiveCard *b) { return a->z > b->z; });
        bool anyAnim = false;
        for (LiveCard *cp : order) {
            LiveCard &card = *cp;
            if (card.dragging || card.engaging) {
                card.hovered = false;
                if (card.dragging) {
                    card.curScale = 1.0;
                    card.curTiltDeg = 0.0;
                }
            } else {
                const QRectF rest = card.target.rect;
                const QRectF grown(rest.topLeft(),
                                   rest.size() * card.hoverScale);
                const bool hit = rest.contains(cur)
                    || (card.hovered && grown.contains(cur));
                if (hit != card.hovered) {
                    card.hovered = hit;
                    card.scaleFrom = card.curScale;
                    card.tiltFrom = card.curTiltDeg;
                    card.scaleTo = hit ? card.hoverScale : 1.0;
                    card.tiltTo = hit ? card.hoverTiltDeg : card.target.angleDeg;
                    card.hoverTl = TimeLine(card.hoverMs);
                    card.hoverAnimating = true;
                }
            }
            if (card.hoverAnimating) {
                card.hoverTl.advance(presentTime);
                const qreal t = qBound(0.0, card.hoverTl.value(), 1.0);
                // OutBack（overshoot 1.2，QML 同参）与 OutCubic 的手工版
                const qreal c1 = 1.2 * 1.70158, c3 = c1 + 1.0;
                const qreal tm1 = t - 1.0;
                const qreal back = 1.0 + c3 * tm1 * tm1 * tm1 + c1 * tm1 * tm1;
                const qreal cubic = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
                card.curScale = card.scaleFrom
                    + (card.scaleTo - card.scaleFrom) * back;
                card.curTiltDeg = card.tiltFrom
                    + (card.tiltTo - card.tiltFrom) * cubic;
                if (card.hoverTl.done())
                    card.hoverAnimating = false;
                anyAnim = true;
            }
            if (card.fadeAnimating) {
                card.fadeTl.advance(presentTime);
                const qreal t = qBound(0.0, card.fadeTl.value(), 1.0);
                card.alpha = card.alphaFrom + (card.alphaTo - card.alphaFrom) * t;
                if (card.fadeTl.done())
                    card.fadeAnimating = false;
                anyAnim = true;
            }
        }
        // 退场 ghost：淡完即彻底释放
        QList<QString> ghostDone;
        for (auto it = m_liveFading.begin(); it != m_liveFading.end(); ++it) {
            LiveCard &card = **it;
            if (card.fadeAnimating) {
                card.fadeTl.advance(presentTime);
                const qreal t = qBound(0.0, card.fadeTl.value(), 1.0);
                card.alpha = card.alphaFrom + (card.alphaTo - card.alphaFrom) * t;
                if (card.fadeTl.done())
                    card.fadeAnimating = false;
            }
            if (!card.fadeAnimating)
                ghostDone.append(it.key());
            anyAnim = true;
        }
        for (const QString &id : ghostDone) {
            releaseLiveCard(*m_liveFading[id]);
            m_liveFading.remove(id);
        }
        if (anyAnim) {
            // 动画卡区域并集重绘（全屏 addRepaintFull 在本机 GPU 是掉帧
            // 大头——卡列只占屏幕一角）
            for (LiveCard *cp2 : order) {
                const LiveCard &c2 = *cp2;
                const QRectF r(c2.target.rect.topLeft(),
                               QSizeF(c2.target.rect.width() * c2.hoverScale,
                                      c2.target.rect.height() * c2.hoverScale));
                effects->addRepaint(r.adjusted(-30, -30, 30, 30).toAlignedRect());
            }
        }
    }

    effects->prePaintScreen(data, presentTime);
}

// 活体直绘走 paintScreen 后置通道（所有窗口画完之后）：不依赖宿主窗
// 判定与它的绘制时机（宿主方案实测会因条带区域无损伤而不重绘=内容
// 冻结）；代价是内容盖在条带 chrome 之上——内容矩形内缩于卡面，边框/
// 标题/图标排不受影响，仅失去背板色调/深度渐变的叠加（后续可在着色器
// 里补）。任何窗口动画播放期间整体让位（飞行窗口会横穿卡列，层序
// 不可与直绘混排）。
void StageAnimEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                  int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    static quint32 s_psCalls = 0;
    if (++s_psCalls % 18000 == 1)
        qCWarning(STAGEANIM_LOG) << "live paintScreen called #" << s_psCalls
                                 << "cards=" << m_liveCards.size()
                                 << "anims=" << m_animations.size();
    effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);

    // ⚠️ 不可再有 m_animations 全局让位闸：卡面视觉整体在特效手里，全局
    // 一让＝动画期间整列卡消失（旧架构只让内容、QML 卡面还在）。改为
    // 逐卡让位——飞行中窗口自己的卡跳过（liveCardPaintable 内判定），
    // 其余卡照画；飞行窗由合成器画在窗层级，交叠瞬间由 engage 淡出遮蔽。
    if (m_liveCards.isEmpty() && m_liveFading.isEmpty())
        return;
    if (effects->activeFullScreenEffect())
        return;
    drawLiveCards(renderTarget, viewport);
}

void StageAnimEffect::prePaintWindow(RenderView *view, EffectWindow *w, WindowPrePaintData &data, std::chrono::milliseconds presentTime)
{
    auto animationIt = m_animations.find(w);
    if (animationIt != m_animations.end()) {
        (*animationIt).timeLine.advance(presentTime);
        data.setTransformed();
    }

    effects->prePaintWindow(view, w, data, presentTime);
}

// 触发时解析该窗口的目标矩形，优先级：
// ① shell 发布的按窗口 id 卡片矩形（窗口从自己的卡片原地长出/收回）
// ② 全局侧栏矩形（kwinrc [Effect-stageanim] Target*）
// ③ 都没有时保持无效，apply() 走原版回落（任务栏图标几何/光标）。
void StageAnimEffect::resolveTarget(EffectWindow *w, StageAnimAnimation &anim,
                                    const QVector<StageTarget> &targets)
{
    anim.target = QRect();
    anim.endScale = -1.0;
    anim.flat = false;

    const QRect geo = w->frameGeometry().toRect();
    if (!targets.isEmpty() && geo.width() > 0) {
        const QString selfId = w->internalId().toString(QUuid::WithoutBraces);
        for (const auto &t : targets) {
            if (t.id != selfId || !t.rect.isValid())
                continue;
            // 等比缩放装进卡片（两轴取小，不拉伸内容——比例失调是红线）
            anim.target = t.rect;
            anim.flat = t.flat;
            anim.endScale = std::clamp(
                std::min(qreal(t.rect.width()) / geo.width(),
                         qreal(t.rect.height()) / geo.height()),
                kCardMinEndScale, kCardMaxEndScale);
            logTarget(m_trace, t.flat ? "card-flat" : "card", selfId, t.rect,
                anim.endScale);
            return;
        }
    }

    anim.target = QRect();
    if (m_target.isValid() && geo.width() > 0) {
        anim.target = m_target;
        anim.endScale = std::clamp(
            qreal(anim.target.width()) / std::max(1, geo.width()),
            kGlobalMinScale, kGlobalMaxScale);
        logTarget(m_trace, "global", w->internalId().toString(QUuid::WithoutBraces),
                  anim.target, anim.endScale);
        return;
    }
    logTarget(m_trace, "fallback", w->internalId().toString(QUuid::WithoutBraces),
              QRect(), -1.0);
}

void StageAnimEffect::apply(EffectWindow *w, int mask, WindowPaintData &data, WindowQuadList &quads)
{
    auto animationIt = m_animations.constFind(w);
    if (animationIt == m_animations.constEnd())
        return;

    // 0 = not minimized, 1 = fully minimized
    const qreal progress = (*animationIt).timeLine.value();
    const qreal t = m_easing.valueForProgress(progress);

    const QRect geo = w->frameGeometry().toRect();

    // 目标矩形与终态缩放比例
    QRectF target;
    qreal endScale = 0.15;
    const bool haveTarget = (*animationIt).endScale > 0
        && (*animationIt).target.isValid();
    if (haveTarget) {
        target = (*animationIt).target;
        endScale = (*animationIt).endScale;
    } else {
        // 原版回落：任务栏图标几何 / 光标最近边缘
        QRect icon = w->iconGeometry().toRect();
        if (icon.isValid() && icon.width() > 0) {
            target = icon;
            endScale = std::clamp(qreal(icon.width()) / std::max(1, geo.width()),
                                  kGlobalMinScale, kGlobalMaxScale);
        } else {
            QPoint pt = cursorPos().toPoint();
            const QRect extG = geo;
            if (extG.contains(pt)) {
                const int d[4] = {pt.x() - extG.x(), extG.right() - pt.x(),
                                  pt.y() - extG.y(), extG.bottom() - pt.y()};
                int di = d[0];
                int which = 0;
                for (int i = 1; i < 4; ++i) {
                    if (d[i] < di) {
                        di = d[i];
                        which = i;
                    }
                }
                switch (which) {
                case 0:
                    pt.setX(extG.x());
                    break;
                case 1:
                    pt.setX(extG.right());
                    break;
                case 2:
                    pt.setY(extG.y());
                    break;
                default:
                    pt.setY(extG.bottom());
                    break;
                }
            } else {
                pt.setX(std::clamp(pt.x(), extG.x(), extG.right()));
                pt.setY(std::clamp(pt.y(), extG.y(), extG.bottom()));
            }
            target = QRect(pt, QSize(0, 0));
        }
    }

    // 窗口中心从自身滑向目标中心；等比缩放；末段 30% 淡出（还原时间线倒放
    // 即先淡入）。在此之上叠加旋转分量：t=1（卡片态）带卡片的 3D 倾斜姿态、
    // t=0 平铺——窗口"从倾斜卡片旋转展开"，而不是生硬的等比放大。
    // WindowVertex 只有 2D 顶点，倾斜用针孔透视投影模拟：绕缩放矩形中心
    // 垂直轴（pivot 与卡片 origin.x = width/2 对齐）转 angleDeg，顶点按
    // 深度 k = focal/(focal−z) 缩放。⚠️ Qt 的 Y 轴朝下，QML 正角绕 Y 旋转时
    // 卡片是"左缘近大、右缘远小"（右缘 z<0 远离观察者），特效必须同向。
    const QPointF fromC = QRectF(geo).center();
    const QPointF toC = target.center();
    const QPointF c = fromC + (toC - fromC) * t;
    const qreal s = 1.0 + (endScale - 1.0) * t;
    // 右侧常驻（m_mirrorTargets）时装卡倾斜取反：镜像后右缘近大，与
    // 卡片的右侧镜像角同姿态（StageCard 的 angleRad 同源取反）
    const qreal tiltSign = m_mirrorTargets ? -1.0 : 1.0;
    const qreal angleDeg = (haveTarget && !(*animationIt).flat)
        ? tiltSign * m_tiltAngle * t : 0.0;
    const qreal rad = qDegreesToRadians(angleDeg);
    const qreal cosR = std::cos(rad);
    const qreal sinR = std::sin(rad);
    const qreal focal = kPerspectiveFocal;
    const qreal pivotX = c.x(); // 中心垂直轴（与卡片 origin.x = width/2 对齐）
    const qreal cy = c.y();

    for (WindowQuad &quad : quads) {
        for (int j = 0; j < 4; ++j) {
            WindowVertex &v = quad[j];
            const qreal gx = geo.x() + v.x();
            const qreal gy = geo.y() + v.y();
            qreal sx = (gx - fromC.x()) * s + c.x();
            qreal sy = (gy - fromC.y()) * s + c.y();
            if (std::abs(angleDeg) > 0.01) {
                const qreal depth = (pivotX - sx) * sinR; // 正角左缘近大，负角（镜像）右缘近大
                const qreal k = focal / (focal - depth);
                sx = pivotX + (sx - pivotX) * cosR * k;
                sy = cy + (sy - cy) * k;
            }
            v.setX(sx - geo.x());
            v.setY(sy - geo.y());
        }
    }

    // 玻璃透明感：透明度从 1（平铺态）全程渐变到 glassOpacity（卡片态）
    // ——飞行途中窗口呈半透明、能透见桌面；restore 时间线倒放即"从玻璃
    // 姿态逐渐凝实"。末段 fade 尾巴在其上收尾（装卡端淡没，防残影）。
    // glassOpacity = 1.0 时该乘子恒为 1，行为与纯淡出版一致。
    const qreal glassMix = 1.0 + (m_glassOpacity - 1.0) * t;
    const qreal fade = std::clamp((1.0 - t) / kFadeTailFraction, 0.0, 1.0);
    data.setOpacity(data.opacity() * glassMix * fade);
}

void StageAnimEffect::postPaintScreen()
{
    auto animationIt = m_animations.begin();
    while (animationIt != m_animations.end()) {
        if ((*animationIt).timeLine.done()) {
            unredirect(animationIt.key());
            animationIt = m_animations.erase(animationIt);
        } else {
            ++animationIt;
        }
    }

    effects->addRepaintFull();

    // Call the next effect.
    effects->postPaintScreen();
}

void StageAnimEffect::slotWindowAdded(EffectWindow *w)
{
    if (m_connected.contains(w))
        return;
    m_connected.insert(w);
    connect(w, &EffectWindow::minimizedChanged, this, [this, w]() {
        if (w->isMinimized()) {
            slotWindowMinimized(w);
        } else {
            slotWindowUnminimized(w);
        }
    });
    // 文件里挂着但窗口此前不存在的活体卡，此刻补建（10s reload 也兜）
    if (!m_livePending.isEmpty())
        QTimer::singleShot(50, this, &StageAnimEffect::reloadLiveCards);
}

void StageAnimEffect::slotWindowDeleted(EffectWindow *w)
{
    m_animations.remove(w);
    m_connected.remove(w);

    // 活体卡生命周期：卡片窗口死亡 → 撤引用并除名
    QList<QString> dead;
    for (auto it = m_liveCards.begin(); it != m_liveCards.end(); ++it) {
        if ((*it)->window == w || (*it)->window.isNull()) {
            releaseLiveCard(**it);
            dead.append(it.key());
        }
    }
    if (!dead.isEmpty()) {
        for (const QString &id : dead)
            m_liveCards.remove(id);
        writeLiveStatus();
    }
}

void StageAnimEffect::slotWindowMinimized(EffectWindow *w)
{
    if (effects->activeFullScreenEffect()) {
        return;
    }

    // targets 文件每触发只读一次（suppress 判定 + 卡片矩形共用同一快照）
    const StageTargetsFile snap = loadStageTargets();
    // suppress 名单 = 实时模式的静默收放（悬停预备/实时恢复）：不建动画，
    // 窗口瞬间消失——视觉上"没发生过"，实时内容与显式动画由此共存
    if (isSuppressed(snap.suppress, w)) {
        return;
    }

    StageAnimAnimation &animation = m_animations[w];
    resolveTarget(w, animation, snap.targets);

    if (animation.timeLine.running()) {
        animation.timeLine.toggleDirection();
    } else {
        animation.visibleRef = EffectWindowVisibleRef(w, EffectWindow::PAINT_DISABLED_BY_MINIMIZE);
        animation.timeLine.setDirection(TimeLine::Forward);
        animation.timeLine.setDuration(m_duration);
        animation.timeLine.setEasingCurve(QEasingCurve::Linear);
    }

    redirect(w);
    effects->addRepaintFull();
}

void StageAnimEffect::slotWindowUnminimized(EffectWindow *w)
{
    if (effects->activeFullScreenEffect()) {
        return;
    }

    const StageTargetsFile snap = loadStageTargets();
    // 同上：suppress 名单内的还原瞬间完成（不播翻转动画）——engage 前由
    // shell 先把目标窗移出名单，翻转动画才可见
    if (isSuppressed(snap.suppress, w)) {
        return;
    }

    StageAnimAnimation &animation = m_animations[w];
    resolveTarget(w, animation, snap.targets);

    if (animation.timeLine.running()) {
        animation.timeLine.toggleDirection();
    } else {
        animation.visibleRef = EffectWindowVisibleRef(w, EffectWindow::PAINT_DISABLED_BY_MINIMIZE);
        animation.timeLine.setDirection(TimeLine::Backward);
        animation.timeLine.setDuration(m_duration);
        animation.timeLine.setEasingCurve(QEasingCurve::Linear);
    }

    redirect(w);
    effects->addRepaintFull();
}

bool StageAnimEffect::isActive() const
{
    if (!m_animations.isEmpty() || !m_liveCards.isEmpty())
        return true;
    for (auto it = m_liveCards.constBegin(); it != m_liveCards.constEnd(); ++it) {
        if (liveCardPaintable(**it))
            return true;
    }
    return false;
}

// ── 活体卡（合成器直绘）──────────────────────────────────────────
// 通道：shell 发布 stage-live.json（每卡终态卡面矩形 + 透视参数）→
// 特效对每张卡的代表窗持 refOffscreenRendering（KWin 起刷新率定时器给
// 隐藏窗补发帧回调——客户端继续渲染，成本等同窗口可见在桌面；无离屏
// 渲染管线、无导出、无跨进程消费者）→ 窗口内容渲进卡面尺寸小 FBO
//（损伤驱动重拍，填充率可忽略）→ 侧栏浮层窗口的 paintWindow 钩子里、
// 条带自绘内容之前，用 QML stage_tilt/stage_round 的着色器孪生画到
// ── 卡面本体（方案"卡进特效"）：元数据 + 铭牌光栅 ──
// v2 协议元数据：QML 只发布静止几何与参数，悬停/展开动画由特效自驱。
struct CardMeta
{
    QString title;
    int count = 1;
    qreal z = 0;
    bool dragging = false, engaging = false, dropHover = false, dwellHint = false;
    qreal hoverScale = 1.18, hoverTiltDeg = 0;
    std::chrono::milliseconds hoverMs{240};
    qreal fanSpacing = 6, tintAlpha = 0.55, borderAlpha = 0.28;
    qreal depthStrength = 0.22, topLight = 0.10;
    qreal fade = 1.0;   // QML slot.opacity（压暗 × 视口边缘渐隐）
    bool closeHot = false;
};

static void applyCardMeta(LiveCard &card, const CardMeta &m)
{
    card.title = m.title;
    card.count = m.count;
    card.z = m.z;
    card.dragging = m.dragging;
    card.engaging = m.engaging;
    card.dropHover = m.dropHover;
    card.dwellHint = m.dwellHint;
    card.hoverScale = m.hoverScale > 1.01 ? m.hoverScale : 1.18;
    card.hoverTiltDeg = m.hoverTiltDeg;
    card.hoverMs = m.hoverMs;
    card.fanSpacing = m.fanSpacing;
    card.depthStrength = m.depthStrength;
    card.topLight = m.topLight;
    // QML 同款色族：静置 rgba(0.05,0.07,0.12,tint)，悬停 ×1.3 提亮
    const qreal ta = qBound(0.05, m.tintAlpha, 0.95);
    const qreal th = qBound(0.05, qMin(0.95, m.tintAlpha * 1.3), 0.95);
    card.tint = QColor(13, 18, 31, int(ta * 255));
    card.tintHover = QColor(26, 33, 51, int(th * 255));
    card.border = QColor(255, 255, 255, int(qBound(0.0, m.borderAlpha, 1.0) * 255));
    card.fade = m.fade;
    card.closeHot = m.closeHot;
}

// 铭牌（标题 + 关闭钮，QML cardHeader 同版式 ×2 超采样）光栅进纹理。
// QImage 行 0 = 顶部，stage-live 着色器按 FBO 朝向（行 0 = 底）采样——
// 上传前 Y 镜像即可复用同一着色器。键变才重绘（标题/计数/卡宽）。
void StageAnimEffect::rasterChrome(LiveCard &card)
{
    const int w = std::max(2, qRound(card.target.rect.width()));
    const int h = std::max(2, qRound(card.target.rect.height()));
    const QString key = card.title + QLatin1Char('|')
        + QString::number(card.count) + QLatin1Char('|')
        + QString::number(w) + QLatin1Char('x') + QString::number(h)
        + QLatin1Char(card.closeHot ? '!' : '.');
    if (key == card.chromeKey && card.chromeTex)
        return;
    card.chromeKey = key;

    QImage img(w * 2, h * 2, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    QFont f = p.font();
    f.setPixelSize(22); // QML pixelSize 11 ×2
    f.setWeight(QFont::Bold);
    p.setFont(f);
    // 标题：头部行（margins 8 + 高 24，QML cardHeader 版式）；右侧让出
    // 关闭钮 20 + 间隙 6；白字 + 1px 黑投影（QML Text.Outline 的近似）
    const QRect tr(16, 16, w * 2 - 16 - 52 * 2 - 8, 48);
    p.setPen(QColor(0, 0, 0, 150));
    p.drawText(tr.translated(0, 2), Qt::AlignLeft | Qt::AlignVCenter, card.title);
    p.setPen(QColor(255, 255, 255, 235));
    p.drawText(tr, Qt::AlignLeft | Qt::AlignVCenter, card.title);
    // 关闭钮：右上 20×20 圆环 + ×（QML cardClose 同位：right margin 8）
    const int csize = 40; // 20 ×2
    const int cx = w * 2 - 16 - csize;
    const int cy = 8 * 2 + (48 - csize) / 2;
    // 悬停红钮（QML cardClose #ef4444 同款）；非悬停 = 白圈描线
    if (card.closeHot) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(239, 68, 68, 235));
        p.drawEllipse(QRect(cx, cy, csize, csize));
        p.setPen(QPen(QColor(255, 255, 255, 235), 3.0));
    } else {
        p.setPen(QPen(QColor(255, 255, 255, 150), 2.4));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRect(cx, cy, csize, csize));
    }
    p.drawLine(cx + 13, cy + 13, cx + csize - 13, cy + csize - 13);
    p.drawLine(cx + csize - 13, cy + 13, cx + 13, cy + csize - 13);
    p.end();

    // 本 KWin 无 GLTexture(QImage) 构造：allocate + glTexSubImage2D 直传
    //（QImage ARGB32_Premultiplied 小端字节序 = BGRA；Y 已镜像匹配 FBO 朝向）
    const QImage flipped = img.mirrored(false, true);
    card.chromeTex = GLTexture::allocate(GL_RGBA8, flipped.size());
    if (card.chromeTex) {
        card.chromeTex->setFilter(GL_LINEAR);
        card.chromeTex->setWrapMode(GL_CLAMP_TO_EDGE);
        card.chromeTex->bind();
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, flipped.width(), flipped.height(),
                        GL_BGRA, GL_UNSIGNED_BYTE, flipped.constBits());
        card.chromeTex->unbind();
    }
}

void StageAnimEffect::reloadLiveCards()
{
    QSet<QString> wanted;
    QVector<std::tuple<QString, LiveCardPose, CardMeta>> entries;
    QFile f(m_livePath);
    if (f.open(QIODevice::ReadOnly)) {
        const auto doc = QJsonDocument::fromJson(f.readAll());
        if (!doc.isNull()) {
            const auto obj = doc.object();
            const auto arr = obj.value(QStringLiteral("cards")).toArray();
            for (const auto &v : arr) {
                const auto o = v.toObject();
                LiveCardPose pose;
                pose.rect = QRectF(o.value(QStringLiteral("x")).toDouble(),
                                   o.value(QStringLiteral("y")).toDouble(),
                                   o.value(QStringLiteral("w")).toDouble(),
                                   o.value(QStringLiteral("h")).toDouble());
                pose.angleDeg = o.value(QStringLiteral("angle")).toDouble();
                pose.yOff = o.value(QStringLiteral("yOff")).toDouble();
                pose.focal = o.value(QStringLiteral("focal")).toDouble();
                pose.radius = o.value(QStringLiteral("radius")).toDouble();
                const QString id = o.value(QStringLiteral("id")).toString();
                if (id.isEmpty() || !pose.rect.isValid())
                    continue;
                if (pose.focal < 100.0)
                    pose.focal = 2200.0;
                // v2 卡面元数据（chrome 由特效同管线绘制）
                CardMeta meta;
                meta.title = o.value(QStringLiteral("title")).toString();
                meta.count = std::max(1, o.value(QStringLiteral("count")).toInt(1));
                meta.z = o.value(QStringLiteral("z")).toDouble();
                meta.dragging = o.value(QStringLiteral("dragging")).toBool();
                meta.engaging = o.value(QStringLiteral("engaging")).toBool();
                meta.dropHover = o.value(QStringLiteral("dropHover")).toBool();
                meta.dwellHint = o.value(QStringLiteral("dwellHint")).toBool();
                meta.hoverScale = o.value(QStringLiteral("hoverScale")).toDouble(1.18);
                meta.hoverTiltDeg = o.value(QStringLiteral("hoverTilt")).toDouble(0.0);
                meta.hoverMs = std::chrono::milliseconds(
                    std::max(80, o.value(QStringLiteral("hoverMs")).toInt(240)));
                meta.fanSpacing = o.value(QStringLiteral("fanSpacing")).toDouble(6.0);
                meta.tintAlpha = o.value(QStringLiteral("cardTint")).toDouble(0.55);
                meta.borderAlpha = o.value(QStringLiteral("cardBorder")).toDouble(0.28);
                meta.depthStrength = o.value(QStringLiteral("cardDepth")).toDouble(0.22);
                meta.topLight = o.value(QStringLiteral("cardTopLight")).toDouble(0.10);
                meta.fade = qBound(0.0, o.value(QStringLiteral("fade")).toDouble(1.0), 1.0);
                meta.closeHot = o.value(QStringLiteral("closeHover")).toBool();
                entries.append({id, pose, meta});
                wanted.insert(id);
            }
        }
    }
    // 心跳超时（shell 死亡/停摆）：撤销全部引用，status 置 inactive。
    // 用文件 mtime 判活（'at' 字段实测间歇落进陈旧值；mtime 由内核在
    // writePath 落盘时盖章，可靠）。25s = 心跳 15s 的 1.7 倍容错
    {
        const QFileInfo info(m_livePath);
        if (!info.exists()
            || QDateTime::currentMSecsSinceEpoch()
                    - info.lastModified().toMSecsSinceEpoch()
                > 25000) {
            wanted.clear();
            entries.clear();
        }
    }
    if (!m_liveEnabled) {
        wanted.clear();
        entries.clear();
    }

    // 掉卡迟滞：发布流里瞬时缺席（桥的 rep 记录抖动/minimized 翻转的那
    // 一拍）不代表卡真没了——立即掉＝"不稳定消失"（淡出→重注册→入场
    // 动画，30 分钟 83 次重注册的真相）。缺席满 350ms 才真掉（侧栏关闭
    // 写空表也走同一迟滞）。
    const qint64 nowSteady = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    QList<QString> drop;
    for (auto it = m_liveCards.begin(); it != m_liveCards.end(); ++it) {
        if (wanted.contains(it.key())) {
            (*it)->absentSinceMs = 0;
            continue;
        }
        if ((*it)->absentSinceMs == 0)
            (*it)->absentSinceMs = nowSteady;
        if (nowSteady - (*it)->absentSinceMs > 350)
            drop.append(it.key());
    }
    for (const QString &id : drop) {
        LiveCard &card = *m_liveCards[id];
        // 退场淡出（QML 卡消失动画的特效侧等价物）：解除离屏引用与损伤
        // 连接（窗口多半已还原/关闭），纹理留存 150ms 渐隐
        detachLiveCard(card);
        card.alphaFrom = card.alpha;
        card.alphaTo = 0.0;
        card.fadeTl = TimeLine(std::chrono::milliseconds(150));
        card.fadeAnimating = true;
        card.hoverAnimating = false;
        m_liveFading.insert(id, m_liveCards[id]);
        m_liveCards.remove(id);
    }

    bool changed = !drop.isEmpty();
    for (const auto &e : entries) {
        const QString &eid = std::get<0>(e);
        auto it = m_liveCards.find(eid);
        if (it == m_liveCards.end()) {
            EffectWindow *w = nullptr;
            const QList<EffectWindow *> all = effects->stackingOrder();
            for (EffectWindow *c : all) {
                if (c->internalId().toString(QUuid::WithoutBraces) == eid) {
                    w = c;
                    break;
                }
            }
            if (!w) {
                m_livePending.insert(eid);
                continue;
            }
            auto card = QSharedPointer<LiveCard>::create();
            card->id = std::get<0>(e);
            card->window = w;
            card->target = std::get<1>(e);
            card->from = card->target; // 首次直接落位（与卡片淡入同拍）
            card->ease = TimeLine(std::chrono::milliseconds(80));
            applyCardMeta(*card, std::get<2>(e));
            card->curTiltDeg = card->target.angleDeg; // 悬停引擎起点 = 静止角
            card->chromeKey.clear(); // 强制首帧光栅铭牌
            // 入场动画（QML 收编入场同款：淡入 + 0.86 长到 1，OutCubic）
            card->alpha = 0.0;
            card->alphaFrom = 0.0;
            card->alphaTo = 1.0;
            card->fadeTl = TimeLine(std::chrono::milliseconds(
                std::max(120, int(std::chrono::duration_cast<std::chrono::milliseconds>(
                    card->hoverMs).count() * 3 / 4))));
            card->fadeAnimating = true;
            card->curScale = 0.86;
            card->scaleFrom = 0.86;
            card->scaleTo = 1.0;
            card->tiltFrom = card->target.angleDeg;
            card->tiltTo = card->target.angleDeg;
            card->hoverTl = TimeLine(card->hoverMs);
            card->hoverAnimating = true;
            card->dirty = true;
            if (w->window()) {
                w->window()->refOffscreenRendering();
                card->offscreenRef = true;
                qCWarning(STAGEANIM_LOG) << "live offscreen-ref ok=" << w->window()->isOffscreenRendering()
                                         << "id" << eid.left(8);
            }
            // ⚠️ 损伤信号必须用内部 Window::damaged（官方 screencast 同款，
            // 客户端每次提交都发射）；EffectWindow::windowDamaged 只在窗口
            // 被绘制的路径上发射——最小化窗永远不触发（"活体间歇性"的真
            // 根因：此前全靠 realloc 抖动偶然触发重拍）
            if (w->window()) {
                card->damageConnection = connect(
                    w->window(), &Window::damaged, this, [this, id = eid](KWin::Window *) {
                        auto it2 = m_liveCards.find(id);
                        if (it2 == m_liveCards.end())
                            return;
                        (*it2)->damageCount++;
                        // 官方 screencast 同款时序：损伤到达（合成器线程、
                        // 非绘制时机）立即重拍——渲染器重入在这里是安全的。
                        // ⚠️ 限频 30fps：损伤信号可达 60-120Hz，每次重拍都
                        // 是整窗场景树渲进 FBO（合成器线程大头）；折进 dirty
                        // 由心跳兜底补拍，活体感无肉眼差异
                        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        if (liveCardPaintable(**it2)
                                && nowMs - (*it2)->lastRenderMs >= 33) {
                            (*it2)->lastRenderMs = nowMs;
                            renderLiveTexture(**it2);
                        } else {
                            (*it2)->dirty = true;
                        }
                        const QRectF r = currentPose(**it2).rect;
                        effects->addRepaint(r.adjusted(-40, -40, 40, 40).toAlignedRect());
                    });
            }
            card->dpr = w->screen() ? w->screen()->scale() : 1.0;
            card->feedPhase = (m_liveFeedCounter++) % 4;
            m_liveCards.insert(eid, card);
            m_livePending.remove(eid);
            renderLiveTexture(*card); // 注册时机（非绘制）先拍一帧
            changed = true;
            qCWarning(STAGEANIM_LOG) << "live card +" << eid
                                     << "caption" << w->caption()
                                     << "rect" << card->target.rect
                                     << "angle" << card->target.angleDeg;
        } else {
            LiveCard &card = **it;
            const bool wasEngaging = card.engaging;
            applyCardMeta(card, std::get<2>(e));
            if (card.engaging && !wasEngaging) {
                // 展开开始：卡面淡出让位给窗口飞行动画（engaging opacity
                // 让位已随 chrome 一起搬进特效）
                card.alphaFrom = card.alpha;
                card.alphaTo = 0.0;
                card.fadeTl = TimeLine(std::chrono::milliseconds(180));
                card.fadeAnimating = true;
            }
            const LiveCardPose &epose = std::get<1>(e);
            const bool poseChanged = card.target.rect != epose.rect
                || std::abs(card.target.angleDeg - epose.angleDeg) > 0.01
                || std::abs(card.target.yOff - epose.yOff) > 0.5
                || std::abs(card.target.radius - epose.radius) > 0.5;
            if (poseChanged) {
                // 16ms 发布节拍下姿态流是逐帧的（含 QML 悬停 OutBack 过冲），
                // 缓动只负责抹平取整抖动——80ms 短跟随；日志只记大位移
                //（逐帧姿态流会灌爆 journal）
                const bool bigMove = (card.target.rect.topLeft()
                        - epose.rect.topLeft()).manhattanLength() > 12
                    || std::abs(card.target.angleDeg - epose.angleDeg) > 2.0
                    || std::abs(card.target.yOff - epose.yOff) > 8.0;
                if (bigMove)
                    qCWarning(STAGEANIM_LOG) << "live pose change" << card.id.left(8)
                                             << card.target.rect << "->" << epose.rect
                                             << "yOff" << card.target.yOff << "->" << epose.yOff;
                card.from = currentPose(card);
                card.target = epose;
                // 16ms = 一帧微平滑：姿态流本身逐帧跟随 QML 动画（含过冲
                // 全程），缓动只抹平取整抖动——滞后压到 1-2 帧（80ms 二次
                // 平滑会在快速动画时肉眼拖尾）
                card.ease = TimeLine(std::chrono::milliseconds(16));
                card.ease.setDirection(TimeLine::Forward);
                card.easing = true;
            }
        }
    }


    if (changed) {
        writeLiveStatus();
        effects->addRepaintFull();
    }
}

// 解除离屏引用与损伤连接但保留纹理（退场 ghost 还要画 150ms）
void StageAnimEffect::detachLiveCard(LiveCard &card)
{
    if (card.window && !card.window.isNull()) {
        if (card.offscreenRef && card.window->window())
            card.window->window()->unrefOffscreenRendering();
        QObject::disconnect(card.damageConnection);
    }
    card.offscreenRef = false;
}

void StageAnimEffect::releaseLiveCard(LiveCard &card)
{
    if (card.window && !card.window.isNull()) {
        if (card.offscreenRef && card.window->window())
            card.window->window()->unrefOffscreenRendering();
        QObject::disconnect(card.damageConnection);
    }
    card.offscreenRef = false;
    card.texture.reset();
    card.fbo.reset();
}

// 窗口内容 → 卡面尺寸小 FBO（PreserveAspectCrop：与静态快照 Image
// fillMode 严格一致，宽高比取小裁边）。渲染走 effects->drawWindow 自建
// target/viewport（offscreeneffect maybeRender 同款嵌套，不经过
// paintWindow 钩子、无递归）。
void StageAnimEffect::renderLiveTexture(LiveCard &card)
{
    EffectWindow *w = card.window;
    if (!w)
        return;
    // FBO 尺寸按静止矩形分配（悬停放大期间姿态矩形逐帧变，跟着分配
    // 会每帧 realloc；纹理按归一化采样，绘制侧拉伸即可）
    const QRectF content = card.target.rect;
    if (content.width() < 2 || content.height() < 2) {
        qCWarning(STAGEANIM_LOG) << "live render skip tiny-content" << card.id.left(8);
        return;
    }
    QRectF client = w->clientGeometry();
    if (client.width() < 2 || client.height() < 2)
        client = w->frameGeometry();
    if (client.width() < 2 || client.height() < 2) {
        qCWarning(STAGEANIM_LOG) << "live render skip tiny-client" << card.id.left(8)
                                 << "client" << w->clientGeometry()
                                 << "frame" << w->frameGeometry();
        return;
    }

    const qreal plateAspect = content.width() / content.height();
    QRectF src = client;
    const qreal winAspect = client.width() / client.height();
    if (winAspect > plateAspect) {
        const qreal cw = client.height() * plateAspect;
        src = QRectF(client.center().x() - cw / 2, client.y(), cw, client.height());
    } else if (winAspect < plateAspect) {
        const qreal ch = client.width() / plateAspect;
        src = QRectF(client.x(), client.center().y() - ch / 2, client.width(), ch);
    }

    const int tw = std::max(2, int(std::lround(content.width() * card.dpr)));
    const int th = std::max(2, int(std::lround(content.height() * card.dpr)));
    if (!card.texture || card.texture->size() != QSize(tw, th)) {
        if (card.texture)
            qCWarning(STAGEANIM_LOG) << "live tex REALLOC" << card.id.left(8)
                                     << card.texture->size() << "->" << QSize(tw, th)
                                     << "rect" << content;
        card.texture = GLTexture::allocate(GL_RGBA8, QSize(tw, th));
        if (!card.texture)
            return;
        card.texture->setFilter(GL_LINEAR);
        card.texture->setWrapMode(GL_CLAMP_TO_EDGE);
        card.fbo = std::make_unique<GLFramebuffer>(card.texture.get());
        card.dirty = true; // 尺寸变化后必须重拍
    }
    if (!card.fbo)
        return;

    // 官方 WindowScreenCastSource::render 同款配方：ItemRenderer 直渲
    // 窗口项 + beginFrame/endFrame 包夹——⚠️ endFrame 是纹理换新的落点，
    // 走 effects->drawWindow 的旧路径不换新最小化窗的客户端缓冲（FBO
    // 恒渲旧帧、损伤计数却照爬的根因）
    RenderTarget renderTarget(card.fbo.get());
    const qreal scale = qreal(tw) / src.width();
    RenderViewport viewport(src, scale, renderTarget, QPoint());
    auto *scene = Compositor::self()->scene();
    // 官方 screencast 的 render() 由流的独立时机调用；从损伤回调等非绘制
    // 时机调用时必须自己把 FBO 绑上（beginFrame 不代劳）——漏绑 =
    // incomplete framebuffer，渲出来全黑（嵌套试验台实测定位）
    GLFramebuffer::pushFramebuffer(card.fbo.get());
    scene->renderer()->beginFrame(renderTarget, viewport);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    scene->renderer()->renderItem(renderTarget, viewport, w->windowItem(),
                                  Scene::PAINT_WINDOW_TRANSFORMED,
                                  Region::infinite(), WindowPaintData{}, {}, {});
    scene->renderer()->endFrame();
    card.renderCount++;
    if (card.renderCount % 2000 == 1) {
        GLubyte px[4] = {0, 0, 0, 0};
        GLubyte tl[4] = {0, 0, 0, 0};
        // 探针必须仍在 push 窗口内（pop 后读外层绑定 = incomplete
        // framebuffer 全黑假象）；⚠️ 此前 push/pop 修复提交漏删旧 pop
        // 造成双 pop 弹穿帧缓冲栈 = 嵌套崩溃真凶（2026-10-04 定位）
        glReadPixels(tw / 2, th / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glReadPixels(int(tw * 0.15), int(th * 0.97), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, tl);
        qCWarning(STAGEANIM_LOG) << "live tex #" << card.renderCount
                                 << "id" << card.id.left(8)
                                 << "center=" << px[0] << px[1] << px[2] << px[3]
                                 << "top=" << tl[0] << tl[1] << tl[2] << tl[3]
                                 << "damage" << card.damageCount;
    }
    GLFramebuffer::popFramebuffer();
    card.dirty = false;
}

bool StageAnimEffect::liveCardPaintable(const LiveCard &card) const
{
    // 动画期间让位给动画管线（还原/装卡飞行的 apply() 负责姿态）；
    // 非最小化（已还原回桌面）不画。
    return !card.window.isNull() && card.window->isMinimized()
        && !card.window->isDeleted() && !m_animations.contains(card.window);
}

void StageAnimEffect::drawLiveCards(const RenderTarget &renderTarget,
                                    const RenderViewport &viewport)
{
    Q_UNUSED(renderTarget);
    // ⚠️ 本函数运行在合成器绘制周期内：只画现成纹理。任何渲染器重入
    //（beginFrame/renderItem/endFrame、drawWindow）在这里都是状态机破坏
    //＝桌面崩溃（2026-10-03 事故元凶）。重拍全部发生在非绘制时机
    //（损伤回调/注册/状态心跳）。

    // 着色器（实例级重试——static 闩锁跨实例共享是黑卡事故元凶，勿回退）
    auto ensureShader = [this](std::unique_ptr<GLShader> &slot, const QString &frag) {
        if (slot && slot->isValid())
            return true;
        slot.reset();
        slot = ShaderManager::instance()->generateShaderFromFile(
            ShaderTrait::MapTexture,
            QStringLiteral(":/stageanim/shaders/stage-live.vert"),
            frag);
        qCWarning(STAGEANIM_LOG) << "shader created:" << frag
                                 << (slot ? (slot->isValid() ? "valid" : "INVALID") : "null");
        if (!slot || !slot->isValid()) {
            slot.reset();
            return false;
        }
        return true;
    };
    const bool shOk = ensureShader(m_liveShader,
                      QStringLiteral(":/stageanim/shaders/stage-live.frag"))
        && ensureShader(m_cardShader,
                      QStringLiteral(":/stageanim/shaders/stage-card.frag"));
    if (!shOk)
        return;

    // z 升序绘制（低者先画被高者盖住；同 z 按发布序稳定排序）
    QVector<QSharedPointer<LiveCard>> order;
    order.reserve(m_liveCards.size());
    for (auto it = m_liveCards.begin(); it != m_liveCards.end(); ++it)
        order.append(it.value());
    std::stable_sort(order.begin(), order.end(),
                     [](const QSharedPointer<LiveCard> &a,
                        const QSharedPointer<LiveCard> &b) { return a->z < b->z; });

    const qreal dpr = viewport.scale();
    const GLboolean blendWas = glIsEnabled(GL_BLEND);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    const GLboolean scissorWas = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST); // 渲染器按损伤区管理剪刀；直绘越区重画无害

    // 单卡绘制：一个覆盖矩形（投影外接框），fanOnly 时传扇叠副本偏移。
    // 返回前不清 GL 状态（循环外统一恢复）。
    auto drawPass = [&](GLShader &sh, const LiveCardPose &pose, qreal ix,
                        qreal iy, qreal iw, qreal ih, const QVector2D &fanOff,
                        bool fanOnly, const QColor &tint, const QColor &border,
                        qreal borderWidth, qreal depthG, qreal topLight,
                        GLTexture *tex, qreal alpha) {
        ShaderBinder binder(&sh);
        sh.setUniform("modelViewProjectionMatrix", viewport.projectionMatrix());
        sh.setUniform("texUnit", 0);
        sh.setUniform("angleRad", float(qDegreesToRadians(pose.angleDeg)));
        sh.setUniform("focal", float(pose.focal * dpr));
        sh.setUniform("yOff", float(pose.yOff * dpr));
        sh.setUniform("camRel", QVector2D(float(iw / 2), float(ih / 2 - pose.yOff * dpr)));
        sh.setUniform("itemSize", QVector2D(float(iw), float(ih)));
        sh.setUniform("cardSize", QVector2D(float(pose.rect.width() * dpr),
                                            float(pose.rect.height() * dpr)));
        GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
        const QList<GLVertex2D> verts = {
            GLVertex2D{QVector2D(float(ix), float(iy)), QVector2D(0, 0)},
            GLVertex2D{QVector2D(float(ix + iw), float(iy)), QVector2D(1, 0)},
            GLVertex2D{QVector2D(float(ix + iw), float(iy + ih)), QVector2D(1, 1)},
            GLVertex2D{QVector2D(float(ix), float(iy + ih)), QVector2D(0, 1)},
        };
        vbo->reset();
        vbo->setVertices(verts);
        vbo->bindArrays();
        if (tex)
            tex->bind();
        if (&sh == m_cardShader.get()) {
            // 卡面整体 pass：chrome 参数（stage-card 独有 uniform）
            sh.setUniform("crad", float(pose.radius * dpr));
            sh.setUniform("fanOff", fanOff);
            sh.setUniform("fanOnly", fanOnly ? 1.0f : 0.0f);
            sh.setUniform("tint", QVector4D(tint.redF(), tint.greenF(),
                                             tint.blueF(), tint.alphaF()));
            sh.setUniform("borderColor", QVector4D(border.redF(), border.greenF(),
                                                   border.blueF(), border.alphaF()));
            sh.setUniform("borderWidth", float(borderWidth * dpr));
            sh.setUniform("depthG", float(depthG));
            sh.setUniform("topLight", float(topLight));
            sh.setUniform("hasContent", tex && !fanOnly ? 1.0f : 0.0f);
        } else {
            sh.setUniform("crad", 0.0f);
        }
        sh.setUniform("alpha", float(alpha));
        vbo->draw(GL_TRIANGLE_FAN, 0, 4);
        if (tex)
            tex->unbind();
    };

    quint32 paintable = 0;
    for (const auto &cp : order) {
        LiveCard &card = *cp;
        card.dpr = dpr;
        if (!liveCardPaintable(card)) {
            if (!card.dragging) // 拖拽卡可能短暂非最小化（交棒过渡）
                continue;
        }
        if (card.alpha <= 0.01 || !card.texture || !card.fbo)
            continue;
        paintable++;
        card.paintCount++;

        // 有限姿态：静止矩形绕 TopLeft 放大（QML transformOrigin 语义；
        // 拖拽跟手矩形 QML 已逐帧发布，这里免悬停缩放）
        LiveCardPose pose = card.target;
        const qreal fadeMul = card.alpha * card.fade; // 入退场 × 压暗/边缘渐隐
        const qreal sc = card.dragging ? 1.0 : card.curScale;
        pose.rect = QRectF(pose.rect.topLeft(),
                           QSizeF(pose.rect.width() * sc, pose.rect.height() * sc));
        pose.angleDeg = card.dragging ? 0.0 : card.curTiltDeg;

        // 投影外接框（tiltProject 前向，全部设备像素）
        const qreal fw = pose.rect.width() * dpr;
        const qreal fh = pose.rect.height() * dpr;
        const qreal focal = pose.focal * dpr;
        const qreal yOff = pose.yOff * dpr;
        const QPointF c = pose.rect.center() * dpr;
        const qreal rad = qDegreesToRadians(pose.angleDeg);
        const qreal sn = std::sin(rad), cs = std::cos(rad);
        qreal minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
        const qreal us[2] = {-fw / 2, fw / 2};
        const qreal vs[2] = {-fh / 2, fh / 2};
        for (const qreal u : us) {
            const qreal k = focal / (focal + u * sn);
            const qreal x = u * cs * k;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            for (const qreal v : vs) {
                const qreal y = (v + yOff) * k;
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
        const qreal margin = 10.0 * dpr;
        const qreal halfW = std::max(maxX, -minX) + margin;
        const qreal halfH = std::max(maxY, -minY) + margin;
        const qreal ix = c.x() - halfW, iy = c.y() - halfH;
        const qreal iw = halfW * 2, ih = halfH * 2;

        rasterChrome(card); // 键变重绘（非绘制时机以外的光栅+上传）

        const QColor mainTint = card.hovered ? card.tintHover : card.tint;
        QColor mainBorder = card.border;
        if (card.dropHover || card.dwellHint)
            mainBorder = QColor(96, 165, 250, 220); // 合并预示蓝（QML 同款色族）

        // 1) 扇叠背板（同应用多窗：min(count-1,2) 张，向左上探出；悬停
        //    间距 ×1.4 = QML"卡片簇吸气"反馈）
        const int fans = std::min(card.count - 1, 2);
        for (int i = fans - 1; i >= 0; --i) {
            const qreal off = (i + 1) * card.fanSpacing
                * (card.hovered ? 1.4 : 1.0) * dpr;
            QColor ft = card.tint;
            ft.setAlphaF(ft.alphaF() * (0.85 - i * 0.25));
            QColor fb = card.border;
            fb.setAlphaF(fb.alphaF() * (0.8 - i * 0.25));
            drawPass(*m_cardShader, pose, ix - off, iy - off, iw, ih,
                     QVector2D(float(-off), float(-off)), true, ft, fb,
                     1.0, 0.0, 0.0, nullptr, fadeMul);
        }

        // 2) 主卡（背板 + 渐变 + 内容 + 边框 + 圆角一体）
        drawPass(*m_cardShader, pose, ix, iy, iw, ih, QVector2D(0, 0), false,
                 mainTint, mainBorder, 1.0, card.depthStrength, card.topLight,
                 card.texture.get(), fadeMul);

        // 3) 铭牌（标题/关闭钮，光栅纹理；无圆角裁形）
        if (card.chromeTex && fadeMul > 0.3)
            drawPass(*m_liveShader, pose, ix, iy, iw, ih, QVector2D(0, 0),
                     false, QColor(), QColor(), 0, 0, 0,
                     card.chromeTex.get(), fadeMul);

        // 落屏探针（节流）
        if (card.paintCount % 300 == 1) {
            GLubyte sp[4] = {255, 0, 255, 255};
            const qreal devH = viewport.renderRect().height() * dpr;
            glReadPixels(int(pose.rect.center().x() * dpr),
                         int(devH - pose.rect.center().y() * dpr),
                         1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sp);
            qCWarning(STAGEANIM_LOG) << "live QUAD" << card.id.left(8)
                                     << "screen-px rgba =" << sp[0] << sp[1]
                                     << sp[2] << sp[3] << "damage" << card.damageCount;
        }
    }
    if (scissorWas)
        glEnable(GL_SCISSOR_TEST);
    if (!blendWas)
        glDisable(GL_BLEND);
    // 退场 ghost（冻结姿态淡出；纹理已无更新源）
    for (auto it = m_liveFading.begin(); it != m_liveFading.end(); ++it) {
        LiveCard &card = **it;
        if (card.alpha <= 0.01 || !card.texture)
            continue;
        LiveCardPose pose = card.target;
        // 与主循环同款投影外接框（缩放已冻结在 curScale）
        const qreal sc2 = card.curScale;
        pose.rect = QRectF(pose.rect.topLeft(),
                           QSizeF(pose.rect.width() * sc2, pose.rect.height() * sc2));
        const qreal fw2 = pose.rect.width() * dpr, fh2 = pose.rect.height() * dpr;
        const qreal focal2 = pose.focal * dpr, yOff2 = pose.yOff * dpr;
        const QPointF c2 = pose.rect.center() * dpr;
        const qreal rad2 = qDegreesToRadians(pose.angleDeg);
        const qreal sn2 = std::sin(rad2), cs2 = std::cos(rad2);
        qreal mnx = 1e18, mxx = -1e18, mny = 1e18, mxy = -1e18;
        for (const qreal u : {-fw2 / 2, fw2 / 2}) {
            const qreal k = focal2 / (focal2 + u * sn2);
            mnx = std::min(mnx, u * cs2 * k);
            mxx = std::max(mxx, u * cs2 * k);
            for (const qreal v : {-fh2 / 2, fh2 / 2}) {
                const qreal y = (v + yOff2) * k;
                mny = std::min(mny, y);
                mxy = std::max(mxy, y);
            }
        }
        const qreal hw = std::max(mxx, -mnx) + 10.0 * dpr;
        const qreal hh = std::max(mxy, -mny) + 10.0 * dpr;
        drawPass(*m_cardShader, pose, c2.x() - hw, c2.y() - hh, hw * 2, hh * 2,
                 QVector2D(0, 0), false, card.tint, card.border, 1.0,
                 card.depthStrength, card.topLight, card.texture.get(),
                 card.alpha * card.fade);
    }
    static quint32 s_pass = 0;
    if (++s_pass % 3000 == 1)
        qCWarning(STAGEANIM_LOG) << "live paint pass #" << s_pass
                                 << "paintable =" << paintable;
}


void StageAnimEffect::writeLiveStatus()
{
    QSaveFile f(m_liveStatusPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    QJsonObject obj;
    obj.insert(QStringLiteral("at"), QDateTime::currentMSecsSinceEpoch());
    obj.insert(QStringLiteral("active"), !m_liveCards.isEmpty());
    // chrome:true = 卡面视觉已由特效接管（QML 侧隐藏视觉只留输入热区）
    obj.insert(QStringLiteral("chrome"), !m_liveCards.isEmpty());
    QJsonArray ids;
    for (auto it = m_liveCards.constBegin(); it != m_liveCards.constEnd(); ++it)
        ids.append(it.key());
    obj.insert(QStringLiteral("cards"), ids);
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    f.commit();
}

LiveCardPose StageAnimEffect::lerpPose(const LiveCardPose &a,
                                                        const LiveCardPose &b,
                                                        qreal t)
{
    LiveCardPose out;
    out.rect = QRectF(a.rect.x() + (b.rect.x() - a.rect.x()) * t,
                      a.rect.y() + (b.rect.y() - a.rect.y()) * t,
                      a.rect.width() + (b.rect.width() - a.rect.width()) * t,
                      a.rect.height() + (b.rect.height() - a.rect.height()) * t);
    out.angleDeg = a.angleDeg + (b.angleDeg - a.angleDeg) * t;
    out.yOff = a.yOff + (b.yOff - a.yOff) * t;
    out.focal = b.focal;
    out.radius = b.radius;
    return out;
}

LiveCardPose StageAnimEffect::currentPose(const LiveCard &card)
{
    if (!card.easing)
        return card.target;
    static const QEasingCurve curve(QEasingCurve::OutCubic);
    return lerpPose(card.from, card.target, curve.valueForProgress(card.ease.value()));
}

} // namespace

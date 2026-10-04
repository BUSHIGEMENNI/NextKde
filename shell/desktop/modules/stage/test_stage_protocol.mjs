// stage-live 协议字段契约测试：发布端（StageSidebarWindow.publishLiveCards）
// 与消费端（vendor stageanim.cpp 的 reloadLiveCards 解析）的 44 个字段名
// 是两侧手抄的——任何 rename 在特效侧静默失配（toDouble(默认) 吞掉）。
// 本测试正则摄取两侧键名，断言"特效解析的每个卡字段都在发布载荷里"。
// 顶层键（at/hidden/cards）单独核对。
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const here = new URL(".", import.meta.url);
const sidebar = readFileSync(
    new URL("StageSidebarWindow.qml", here), "utf8");
const effect = readFileSync(
    new URL("../../../../vendor/kwin-effects-stageanim/src/stageanim.cpp", here),
    "utf8");

// ── 发布端：publishLiveCards 的 out.push({...}) 对象里的顶层键 ──
// （先剥行注释再取键——行尾注释不带逗号会把下一键吞进同一段）
const pushMatch = sidebar.match(/out\.push\(\{([\s\S]*?)\}\)/);
assert(pushMatch, "publishLiveCards out.push block not found");
const noComments = pushMatch[1]
    .split("\n").map(l => l.replace(/\/\/.*$/, "")).join("\n");
const published = new Set(["id", ...noComments
    .split(",")
    .map(kv => kv.trim().split(":")[0].trim())
    .filter(k => /^[A-Za-z_][A-Za-z0-9_]*$/.test(k))]);

// ── 消费端：reloadLiveCards 解析块里 o.value(QStringLiteral("...")) ──
const reloadBlock = effect.slice(
    effect.indexOf("void StageAnimEffect::reloadLiveCards"),
    effect.indexOf("bool StageAnimEffect::expireAbsentLiveCards"));
assert(reloadBlock.length > 0, "reloadLiveCards block not found");
const parsed = [...reloadBlock.matchAll(
    /o\.value\(QStringLiteral\("([A-Za-z_][A-Za-z0-9_]*)"\)\)/g)]
    .map(m => m[1]);
const parsedCard = parsed.filter(k => k !== "id" && k !== "winId");

let cases = 0;
for (const k of parsedCard) {
    cases++;
    assert(published.has(k),
        `effect parses "${k}" but publishLiveCards does not send it`);
}
assert(parsedCard.length >= 30,
    `suspiciously few parsed fields (${parsedCard.length}) — parser moved?`);
assert(published.has("winId"), "v3 winId field missing from publish");
cases += 2;

console.log(`stage-protocol: ${cases} checks passed `
    + `(${parsedCard.length} card fields verified)`);

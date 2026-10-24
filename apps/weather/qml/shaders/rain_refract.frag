#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D source;
layout(binding = 2) uniform sampler2D waterMap;
// 批 12 余项①：折射只在天空区生效。掩码为全窗白底 + 卡片/侧栏黑矩形；
// 掩码为 0 处输出未折射的天空（卡片底下仍透天空，但不透水珠）。
layout(binding = 3) uniform sampler2D maskSource;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 surfaceSize;
    vec2 mapRepeat;
    float strength;
    float specular;
    float fog;
    float filmDim;
};

void main()
{
    vec2 uv = qt_TexCoord0;
    // 水图按窗口尺寸平铺；512² 贴图 → uv * (w/512, h/512)。
    vec4 w = texture(waterMap, uv * mapRepeat);

    // 水图带"雨膜"底色（A=0.55, B=1），采样值是预乘 alpha 的，先还原非预乘。
    // 雨膜：B=1 处为有膜，滑落水珠擦过 → B=0 → 亮带（批 12 第 4 项）。
    float cov = w.a;
    vec2 rg = cov > 0.001 ? w.rg / cov : vec2(0.5);
    float film = cov > 0.001 ? w.b / cov : 0.0;

    // 折射：水图 R=y 偏移 / G=x 偏移（0.5 为零点），A=覆盖率。
    // 偏移幅度 ∝ 距圆心距离（边缘最强、中心几乎不偏）——透镜查表，不是逐像素数学。
    vec2 offset = (rg - vec2(0.5)) * strength * cov;
    vec3 col = texture(source, uv + offset).rgb;

    // 单点镜面高光（光源左上 §2.7）：由偏移向量还原法线，只留一个偏心亮点。
    vec2 nxy = (rg - vec2(0.5)) * 2.0;
    vec3 n = normalize(vec3(nxy.x, nxy.y, 0.85));
    vec3 lightDir = normalize(vec3(-0.42, -0.52, 0.75));
    float spec = pow(max(dot(n, lightDir), 0.0), 28.0);
    col += vec3(1.0) * spec * specular * cov;

    // 极淡雾层（水珠内部），不影响水珠外的天空。
    col = mix(col, vec3(0.85, 0.91, 1.0), cov * fog);

    // 雨膜压暗（水珠外）：滑落水珠擦过处 film=0 → 露出更亮的天空。
    col *= 1.0 - film * (1.0 - cov) * filmDim;

    // 卡片/侧栏区域不折射：掩码 0 → 未偏移的天空原样输出（不含高光/雾）。
    vec3 plain = texture(source, uv).rgb;
    vec3 masked = mix(plain, col, texture(maskSource, uv).r);

    fragColor = vec4(masked, 1.0) * qt_Opacity;
}

#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D source;
layout(binding = 2) uniform sampler2D waterMap;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 surfaceSize;
    vec2 mapRepeat;
    float strength;
    float specular;
    float fog;
};

void main()
{
    vec2 uv = qt_TexCoord0;
    // 水图按窗口尺寸平铺；512² 贴图 → uv * (w/512, h/512)。
    vec4 w = texture(waterMap, uv * mapRepeat);

    // 折射：水图 R=y 偏移 / G=x 偏移（0.5 为零点），A=覆盖率。
    // 偏移幅度 ∝ 距圆心距离（边缘最强、中心几乎不偏）——透镜查表，不是逐像素数学。
    vec2 offset = (w.rg - vec2(0.5)) * strength * w.a;
    vec3 col = texture(source, uv + offset).rgb;

    // 单点镜面高光（光源左上 §2.7）：由偏移向量还原法线，只留一个偏心亮点。
    vec2 nxy = (w.rg - vec2(0.5)) * 2.0;
    vec3 n = normalize(vec3(nxy.x, nxy.y, 0.85));
    vec3 lightDir = normalize(vec3(-0.42, -0.52, 0.75));
    float spec = pow(max(dot(n, lightDir), 0.0), 28.0);
    col += vec3(1.0) * spec * specular * w.a;

    // 极淡雾层（水珠内部），不影响水珠外的天空。
    col = mix(col, vec3(0.85, 0.91, 1.0), w.a * fog);

    fragColor = vec4(col, 1.0) * qt_Opacity;
}

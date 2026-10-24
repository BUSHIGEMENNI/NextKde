#version 440

layout(location = 0) in vec4 qt_Vertex;
layout(location = 0) out vec2 qt_TexCoord0;

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
    qt_TexCoord0 = qt_Vertex.xy / max(surfaceSize, vec2(1.0));
    gl_Position = qt_Matrix * qt_Vertex;
}

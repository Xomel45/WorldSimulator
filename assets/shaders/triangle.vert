#version 460

// Тестовый треугольник — позиции захардкожены, vertex buffer не нужен
const vec2 kPositions[3] = {
    vec2( 0.0, -0.5),
    vec2( 0.5,  0.5),
    vec2(-0.5,  0.5),
};

// Цвета в тематике хоррора: белый-призрак, тёмно-красный, пепельный
const vec3 kColors[3] = {
    vec3(0.92, 0.90, 0.88),  // почти белый (призрак)
    vec3(0.72, 0.04, 0.04),  // кровавый красный
    vec3(0.45, 0.42, 0.50),  // мертвенный серо-фиолетовый
};

layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = vec4(kPositions[gl_VertexIndex], 0.0, 1.0);
    fragColor   = kColors[gl_VertexIndex];
}

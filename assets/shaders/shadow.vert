#version 460

// Depth-only проход карты теней: позиция в клип света, больше ничего.
layout(push_constant) uniform PC {
    mat4 lightViewProj;
    mat4 model;
};

layout(location = 0) in vec4 inPos;

void main() {
    gl_Position = lightViewProj * model * vec4(inPos.xyz, 1.0);
}

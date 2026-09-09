#pragma once

#include <glm/glm.hpp>

namespace wsim {

inline constexpr int kMaxPointLights = 8;  // = WSIM_MAX_POINT_LIGHTS в lighting.glsl

// UBO layout — std140, зеркалит LightsUBO в assets/shaders/include/lighting.glsl
// (set = 0, binding = 1, fragment stage).
struct LightsData {
    glm::vec4  ambient{};                            // rgb, w = интенсивность
    glm::vec4  spotPosCosIn{};                       // xyz = позиция, w = cos(inner)
    glm::vec4  spotDirCosOut{0.f, 0.f, -1.f, 0.f};   // xyz = направление, w = cos(outer)
    glm::vec4  spotColor{};                          // rgb, w = интенсивность (0 = выкл)
    glm::vec4  pointPosRadius[kMaxPointLights]{};    // xyz = позиция, w = радиус
    glm::vec4  pointColorInt[kMaxPointLights]{};     // rgb, w = интенсивность
    glm::ivec4 counts{};                             // x = число точечных источников
    glm::vec4  fogColorDensity{};                    // rgb = цвет тумана, w = базовая плотность
    glm::vec4  fogParams{};                          // x = height falloff, y = время (с), zw резерв
};

}  // namespace wsim

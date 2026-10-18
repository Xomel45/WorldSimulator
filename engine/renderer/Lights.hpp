#pragma once

#include "renderer/Shadows.hpp"

#include <glm/glm.hpp>

namespace wsim {

inline constexpr int kMaxPointLights = 8;  // = WSIM_MAX_POINT_LIGHTS в lighting.glsl

// UBO layout — std140, зеркалит LightsUBO в assets/shaders/include/lighting.glsl
// (set = 0, binding = 1, fragment stage).
struct LightsData {
    glm::vec4  ambient{};                            // rgb, w = интенсивность
    // Солнце: направленный свет без позиции/затухания (для террейна — иначе
    // холмы не читаются рельефом, только силуэтом). sunDir — уже итоговое L
    // (направление К солнцу, нормализовано), не позиция источника.
    glm::vec4  sunDir{0.f, 1.f, 0.f, 0.f};
    glm::vec4  sunColorIntensity{};                   // rgb, w = интенсивность (0 = выкл, как у spotColor.w)
    glm::vec4  spotPosCosIn{};                       // xyz = позиция, w = cos(inner)
    glm::vec4  spotDirCosOut{0.f, 0.f, -1.f, 0.f};   // xyz = направление, w = cos(outer)
    glm::vec4  spotColor{};                          // rgb, w = интенсивность (0 = выкл)
    glm::vec4  pointPosRadius[kMaxPointLights]{};    // xyz = позиция, w = радиус
    glm::vec4  pointColorInt[kMaxPointLights]{};     // rgb, w = интенсивность
    glm::ivec4 counts{};                             // x = число точечных источников
    glm::vec4  fogColorDensity{};                    // rgb = цвет тумана, w = базовая плотность
    glm::vec4  fogParams{};                          // x = height falloff, y = время (с), zw резерв
    // Каскадные тени от солнца (см. Shadows.hpp, карта -- binding 4)
    glm::mat4  cascadeVP[kShadowCascades]{};         // мир -> клип света, по каскадам
    glm::vec4  cascadeSplit{};                       // дальние границы каскадов по глубине вида
    glm::vec4  cascadeTexel{};                       // размер текселя каскада в юнитах мира
    glm::vec4  shadowParams{};                       // x = тени включены, y = 1/размер карты, z = смещение глубины
};

}  // namespace wsim

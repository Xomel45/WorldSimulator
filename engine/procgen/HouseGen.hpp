#pragma once

#include "procgen/MeshBuilder.hpp"

#include <cstdint>

namespace wsim::procgen {

// Параметры домика. Начало координат -- центр основания на земле, фасад (дверь) смотрит в +Z.
struct HouseParams {
    float width      = 4.0f;   // по X
    float depth      = 5.0f;   // по Z
    float wallHeight = 2.6f;
    float roofHeight = 1.8f;
    float overhang   = 0.35f;
    bool  ridgeAlongZ = true;
    bool  chimney     = true;
    int   windows     = 2;      // на боковую стену, 0..3
    glm::vec3 wall   {0.78f, 0.70f, 0.56f};
    glm::vec3 roof   {0.52f, 0.22f, 0.16f};
    glm::vec3 trim   {0.30f, 0.22f, 0.15f};  // цоколь, косяки, труба
    glm::vec3 glass  {0.16f, 0.24f, 0.30f};
};

// Детерминированная вариация по зерну (размеры, пропорции крыши, палитра).
[[nodiscard]] HouseParams houseVariant(std::uint32_t seed);

// Дом из боксов и призм: цоколь, стены, крыша со свесом, дверь с косяком, окна, труба.
// Всё замкнуто и с внешними нормалями -- годится и для растеризации, и для теней.
[[nodiscard]] MeshData buildHouse(const HouseParams& p);

}  // namespace wsim::procgen

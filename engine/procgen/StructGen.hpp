#pragma once

#include "procgen/MeshBuilder.hpp"

namespace wsim::procgen {

// Модели построек и снаряжения существ (плоская закраска, из боксов, как дома). Все в единицах мира; снаряжение -- в системе
// координат существа (морда +Z, спина +Y), его размещает и масштабирует по размеру существа Application.
[[nodiscard]] MeshData buildPlank();           // настил моста на одну клетку 2x2, верх на y = 0
[[nodiscard]] MeshData buildBarricade();       // заслон вдоль +X длиной 3.6 из кольев и жердей
[[nodiscard]] MeshData buildTrap();            // ловушка: плита с шипами, радиус ~0.7
[[nodiscard]] MeshData buildSpear();           // копьё вдоль +Z
[[nodiscard]] MeshData buildShield();          // щит в плоскости YZ (носится сбоку)
[[nodiscard]] MeshData buildLogs(int count);   // связка брёвен count = 1..4 (на спине, вдоль X)

}  // namespace wsim::procgen

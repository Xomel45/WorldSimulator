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
[[nodiscard]] MeshData buildStockpile();       // склад: помост, штабель брёвен и навес на столбах
[[nodiscard]] MeshData buildHut();             // хижина: бревенчатые стены, двускатная крыша, дверной проём (дверь к +Z), радиус ~1.4
[[nodiscard]] MeshData buildCampfire();        // костёр: кольцо камней, поленья и пламя (высота пламени ~1 при полном топливе; масштаб по Y)
[[nodiscard]] MeshData buildBanner(int tribe);     // знамя племени 1..3 (красное, синее, жёлтое): шест и полотнище, высота ~1.2
[[nodiscard]] MeshData buildBucket();          // ведро с дужкой (носится сбоку)
[[nodiscard]] MeshData buildLogs(int count);   // связка брёвен count = 1..4 (на спине, вдоль X)

}  // namespace wsim::procgen

#pragma once

#include "sim/Terrain.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace wsim {

enum class TreeSpecies : std::uint8_t { Oak, Birch };
inline constexpr int kTreeVariants = 4;  // моделей на вид

struct TreeInstance {
    float       x = 0, z = 0;
    float       scale = 1.0f;     // 0.8..1.3
    float       yaw = 0.0f;       // градусы
    TreeSpecies species = TreeSpecies::Oak;
    std::uint8_t variant = 0;     // 0..kTreeVariants-1
};

// Деревья -- чистая функция от сида рельефа и координат, как и сам рельеф: ничего не хранится, не сохраняется
// и не нужно "генерировать мир" заранее. Мир разбит на ячейки kCell x kCell; в каждой не больше одного
// дерева (со случайным смещением внутри), шанс зависит от плавного шума "лесистости" -- получаются рощи и
// поляны. Вид: берёзы там, где ниже и "влажнее" (и в отдельных рощах), дубы -- остальное.
// Дерево есть, только если его место годится: суша выше берега, не круче склона и ниже снеговой границы;
// правки игрока (холм под деревом, море) это пересчитывают сами, раз проверка идёт по текущей высоте.
class Trees {
public:
    static constexpr float kCell       = 7.0f;
    static constexpr float kTrunkRadius = 0.45f;  // ствол, о который упираются существа
    static constexpr float kMinHeight  = 0.9f;    // ниже -- пляж/вода
    static constexpr float kMaxHeight  = 10.0f;   // выше -- голые скалы и снег
    static constexpr float kMaxSlope   = 0.55f;   // круче -- деревья не растут

    // Занято ли место (дом, куст с едой): true -- дерево там не растёт. Радиус -- запас на крону.
    using Blocker = std::function<bool(float x, float z, float radius)>;

    // Деревья с корнями в прямоугольнике [x0,x1) x [z0,z1).
    [[nodiscard]] static std::vector<TreeInstance> inRect(const Terrain& terrain, float x0, float z0, float x1, float z1,
                                                          const Blocker* blocked = nullptr);

    // Попадает ли точка в ствол дерева (для существ и обхода препятствий). Без blocker: невидимые
    // деревья под домами остались бы препятствиями -- мелочь, зато дёшево.
    [[nodiscard]] static bool blocksPoint(const Terrain& terrain, float x, float z, float radius = kTrunkRadius);

    // Лесистость 0..1 в точке (для тестов и отладки).
    [[nodiscard]] static float density(std::uint32_t seed, float x, float z);
};

}  // namespace wsim

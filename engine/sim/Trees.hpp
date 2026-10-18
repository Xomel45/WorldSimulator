#pragma once

#include "sim/Terrain.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace wsim {

// Дуб и берёза -- умеренный пояс; ель (ярусы-конусы) -- тайга, тундра и холодные склоны; сосна (высокий голый ствол,
// крона шапкой) -- боры на суховатой стороне холодного и умеренного пояса; акация -- саванна; кактус -- пустыня;
// пальма -- джунгли (вместе с очень крупными дубами). Порядок значений -- номер вида в процедурных моделях (TreeGen).
enum class TreeSpecies : std::uint8_t { Oak, Birch, Spruce, Acacia, Cactus, Palm, Pine };
inline constexpr int kTreeSpeciesCount = 7;
inline constexpr int kTreeVariants     = 4;  // моделей на вид

struct TreeInstance {
    float       x = 0, z = 0;
    float       scale = 1.0f;     // 0.6..1.7 (карликовые сосны тундры -- мельче, джунглевые дубы -- крупнее)
    float       yaw = 0.0f;       // градусы
    TreeSpecies species = TreeSpecies::Oak;
    std::uint8_t variant = 0;     // 0..kTreeVariants-1
    std::int32_t cx = 0, cz = 0;  // ячейка Trees::kCell (по ней дерево срубается: Terrain::fellTree)
};

// Деревья -- чистая функция от сида рельефа и координат, как и сам рельеф: ничего не хранится, не сохраняется
// и не нужно "генерировать мир" заранее. Мир разбит на ячейки kCell x kCell; в каждой не больше одного
// дерева (со случайным смещением внутри), шанс зависит от плавного шума "лесистости" -- получаются рощи и
// поляны. Вид и густота -- по биому (sim/Biomes.hpp): лес и джунгли густые, тайга -- сосны, равнины, тундра и саванна --
// редкие одиночки, в пустыне почти одни кактусы. Границы биомов у каждой ячейки слегка "дрожат", поэтому
// переход постепенный, а не по линейке.
// Дерево есть, только если его место годится: суша выше берега, не круче склона и ниже снеговой границы;
// правки игрока (холм под деревом, море) это пересчитывают сами, раз проверка идёт по текущей высоте.
class Trees {
public:
    static constexpr float kCell       = 7.0f;
    static constexpr float kTrunkRadius = 0.45f;  // ствол, о который упираются существа
    static constexpr float kMinHeight  = 0.9f;    // ниже -- пляж/вода
    static constexpr float kMaxHeight  = 10.0f;   // выше -- голые скалы и снег (хвойные забираются до kMaxHeightConifer)
    static constexpr float kMaxHeightConifer = 13.0f;
    static constexpr float kMinTemperature = 0.07f;  // холоднее (с поправкой на высоту) деревья не растут -- лёд
    static constexpr float kMaxSlope   = 0.55f;   // круче -- деревья не растут

    // Занято ли место (дом, куст с едой): true -- дерево там не растёт. Радиус -- запас на крону.
    using Blocker = std::function<bool(float x, float z, float radius)>;

    // Деревья с корнями в прямоугольнике [x0,x1) x [z0,z1).
    [[nodiscard]] static std::vector<TreeInstance> inRect(const Terrain& terrain, float x0, float z0, float x1, float z1,
                                                          const Blocker* blocked = nullptr);

    // Попадает ли точка в ствол дерева (для существ и обхода препятствий). Без blocker: невидимые
    // деревья под домами остались бы препятствиями -- мелочь, зато дёшево.
    [[nodiscard]] static bool blocksPoint(const Terrain& terrain, float x, float z, float radius = kTrunkRadius);

    // Ближайшее стоящее (не срубленное, годное по рельефу) дерево не дальше radius; false -- нет.
    [[nodiscard]] static bool nearest(const Terrain& terrain, float x, float z, float radius, TreeInstance& out);

    // Лесистость 0..1 в точке (для тестов и отладки).
    [[nodiscard]] static float density(std::uint32_t seed, float x, float z);
};

}  // namespace wsim

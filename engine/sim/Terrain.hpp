#pragma once

#include "sim/Genome.hpp"  // Rng

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wsim {

// Процедурный рельеф: чистая функция высоты от (x, z), без Vulkan/GLFW
// зависимостей и без хранимой сетки -- та же логика "derived/pure-function",
// что у Creature::awayFromHome()/boredomLevel(). Не завязан на World: пока
// это чисто визуальный/географический слой, без геймплейного сопряжения
// (существа не избегают воды, высота не влияет на needs) -- Application
// владеет и Terrain, и World независимо, как двумя отдельными системами.
// Реки сознательно не реализованы в этом раунде -- отдельная задача.
class Terrain {
public:
    static constexpr float kSeaLevel = 0.0f;  // относительно heightAt()

    Terrain(float halfSize, std::uint32_t seed);

    // Прямоугольник в мире (XZ), затронутый правкой: рендер перестраивает чанки под ним.
    struct Rect { float x0, z0, x1, z1; };

    // O(1), можно звать сколько угодно раз откуда угодно -- нет кэшируемой
    // сетки, разрешение меша (забота рендера) полностью не завязано на
    // генерацию (забота сима). Высота = процедурная основа + правки игрока
    // (см. ниже); пока правок нет, цена та же, что у чистой основы.
    [[nodiscard]] float heightAt(float x, float z) const noexcept;
    [[nodiscard]] float baseHeightAt(float x, float z) const noexcept;  // без правок

    // Правки игрока (силы бога) -- разреженная сетка поправок высоты (шаг
    // kEditCell), поверх чистой функции; сгенерированная основа не меняется.
    // Кисть с гладким спадом к краю radius. amount -- прибавка высоты в центре.
    Rect raise(float x, float z, float radius, float amount);
    // Подтягивает высоту к target с силой rate (0..1 за вызов в центре кисти).
    Rect flatten(float x, float z, float radius, float target, float rate);
    [[nodiscard]] bool edited() const noexcept { return !m_edits.empty(); }
    static constexpr float kEditCell = 1.0f;
    static constexpr float kMaxEdit  = 60.0f;  // предел суммарной поправки по модулю
    [[nodiscard]] bool  isUnderwater(float x, float z) const noexcept { return heightAt(x, z) < kSeaLevel; }
    [[nodiscard]] float halfSize() const noexcept { return m_halfSize; }
    [[nodiscard]] std::uint32_t seed() const noexcept { return m_seed; }

    // Сохранения: правки игрока плитками (только ненулевые). Основу рельефа восстанавливает сид.
    struct EditTileData {
        int                tx = 0, tz = 0;
        std::vector<float> values;  // kEditTile * kEditTile
    };
    static constexpr int kEditTileSize = 32;  // = kEditTile (она приватная)
    [[nodiscard]] std::vector<EditTileData> exportEdits() const;
    // false -- плитка некорректного размера (битое сохранение); правки уже применённых плиток остаются.
    bool importEdits(const std::vector<EditTileData>& tiles);

private:
    float                 m_halfSize;
    std::uint32_t         m_seed = 0;
    // Поправки высоты по вершинам целочисленной сетки, плитками 32x32.
    static constexpr int kEditTile = 32;
    struct EditTile { std::array<float, kEditTile * kEditTile> v{}; };
    std::unordered_map<std::int64_t, EditTile> m_edits;
    [[nodiscard]] float editAt(int ix, int iz) const noexcept;
    float&              editRef(int ix, int iz);
    [[nodiscard]] float editHeightAt(float x, float z) const noexcept;
    std::array<int, 512>  m_permutation;  // адаптировано из ../AlphaCraft/src/world/PerlinNoise, но перетасовано через wsim::Rng (mt19937), не default_random_engine

    [[nodiscard]] float noise2D(float x, float z) const noexcept;
    [[nodiscard]] float octaveNoise2D(float x, float z, int octaves, float persistence) const noexcept;
};

}  // namespace wsim

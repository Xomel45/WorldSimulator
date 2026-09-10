#pragma once

#include "sim/Genome.hpp"  // Rng

#include <array>
#include <cstdint>

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

    // O(1), можно звать сколько угодно раз откуда угодно -- нет кэшируемой
    // сетки, разрешение меша (забота рендера) полностью не завязано на
    // генерацию (забота сима).
    [[nodiscard]] float heightAt(float x, float z) const noexcept;
    [[nodiscard]] bool  isUnderwater(float x, float z) const noexcept { return heightAt(x, z) < kSeaLevel; }
    [[nodiscard]] float halfSize() const noexcept { return m_halfSize; }

private:
    float                 m_halfSize;
    std::array<int, 512>  m_permutation;  // адаптировано из ../AlphaCraft/src/world/PerlinNoise, но перетасовано через wsim::Rng (mt19937), не default_random_engine

    [[nodiscard]] float noise2D(float x, float z) const noexcept;
    [[nodiscard]] float octaveNoise2D(float x, float z, int octaves, float persistence) const noexcept;
};

}  // namespace wsim

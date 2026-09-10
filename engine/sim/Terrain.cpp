#include "sim/Terrain.hpp"

#include <algorithm>
#include <cmath>

namespace wsim {

namespace {

// Два слоя шума, обе через одну и ту же fractal-функцию (октавы) -- второй
// алгоритм не нужен: низкочастотный/высокоамплитудный слой лепит форму
// континентов/океанских впадин, высокочастотный сверху -- локальные холмы.
constexpr float kContinentFrequency   = 0.012f;
constexpr int   kContinentOctaves     = 3;
constexpr float kContinentPersistence = 0.5f;
constexpr float kContinentAmplitude   = 6.0f;

constexpr float kDetailFrequency   = 0.12f;
constexpr int   kDetailOctaves     = 4;
constexpr float kDetailPersistence = 0.5f;
constexpr float kDetailAmplitude   = 1.5f;

// Смещение вверх: суши должно быть заметно больше воды (мир для поселений,
// не архипелаг) -- при симметричном шуме sea level=0 иначе делил бы карту
// примерно поровну.
constexpr float kHeightBias = 1.0f;

float fade(float t) noexcept { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
float lerp(float t, float a, float b) noexcept { return a + t * (b - a); }

float grad(int hash, float x, float y) noexcept {
    const int   h = hash & 15;
    const float u = h < 8 ? x : y;
    const float v = h < 4 ? y : (h == 12 || h == 14 ? x : 0.0f);
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

}  // namespace

// Адаптировано из ../AlphaCraft/src/world/PerlinNoise -- та же классическая
// схема (fade/lerp/grad + перестановочная таблица), но таблица тасуется
// через wsim::Rng (mt19937), а не std::default_random_engine, чтобы
// сохранить единый RNG-конвеншн проекта.
Terrain::Terrain(float halfSize, std::uint32_t seed) : m_halfSize(halfSize) {
    std::array<int, 256> p{};
    for (int i = 0; i < 256; ++i) p[i] = i;

    Rng rng(seed);
    std::shuffle(p.begin(), p.end(), rng);

    for (int i = 0; i < 256; ++i) {
        m_permutation[i]       = p[i];
        m_permutation[256 + i] = p[i];
    }
}

float Terrain::noise2D(float x, float z) const noexcept {
    const int X = static_cast<int>(std::floor(x)) & 255;
    const int Z = static_cast<int>(std::floor(z)) & 255;

    x -= std::floor(x);
    z -= std::floor(z);

    const float u = fade(x);
    const float v = fade(z);

    const int A  = m_permutation[static_cast<std::size_t>(X)] + Z;
    const int AA = m_permutation[static_cast<std::size_t>(A)];
    const int AB = m_permutation[static_cast<std::size_t>(A) + 1];
    const int B  = m_permutation[static_cast<std::size_t>(X) + 1] + Z;
    const int BA = m_permutation[static_cast<std::size_t>(B)];
    const int BB = m_permutation[static_cast<std::size_t>(B) + 1];

    return lerp(v,
        lerp(u, grad(m_permutation[static_cast<std::size_t>(AA)], x, z),
                grad(m_permutation[static_cast<std::size_t>(BA)], x - 1.0f, z)),
        lerp(u, grad(m_permutation[static_cast<std::size_t>(AB)], x, z - 1.0f),
                grad(m_permutation[static_cast<std::size_t>(BB)], x - 1.0f, z - 1.0f)));
}

float Terrain::octaveNoise2D(float x, float z, int octaves, float persistence) const noexcept {
    float total = 0.0f, frequency = 1.0f, amplitude = 1.0f, maxValue = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        total += noise2D(x * frequency, z * frequency) * amplitude;
        maxValue += amplitude;
        amplitude *= persistence;
        frequency *= 2.0f;
    }
    return total / maxValue;
}

float Terrain::heightAt(float x, float z) const noexcept {
    const float continental = octaveNoise2D(x * kContinentFrequency, z * kContinentFrequency, kContinentOctaves,
                                             kContinentPersistence) *
                               kContinentAmplitude;
    const float detail =
        octaveNoise2D(x * kDetailFrequency, z * kDetailFrequency, kDetailOctaves, kDetailPersistence) *
        kDetailAmplitude;
    return kHeightBias + continental + detail;
}

}  // namespace wsim

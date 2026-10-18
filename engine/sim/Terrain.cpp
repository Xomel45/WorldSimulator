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
Terrain::Terrain(float halfSize, std::uint32_t seed) : m_halfSize(halfSize), m_seed(seed) {
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

float Terrain::baseHeightAt(float x, float z) const noexcept {
    const float continental = octaveNoise2D(x * kContinentFrequency, z * kContinentFrequency, kContinentOctaves,
                                             kContinentPersistence) *
                               kContinentAmplitude;
    const float detail =
        octaveNoise2D(x * kDetailFrequency, z * kDetailFrequency, kDetailOctaves, kDetailPersistence) *
        kDetailAmplitude;
    return kHeightBias + continental + detail;
}

namespace {
std::int64_t tileKey(int tx, int tz) noexcept {
    return (static_cast<std::int64_t>(tx) << 32) ^ static_cast<std::uint32_t>(tz);
}
int floorDiv(int a, int b) noexcept { return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0); }
}  // namespace

float Terrain::editAt(int ix, int iz) const noexcept {
    const int tx = floorDiv(ix, kEditTile), tz = floorDiv(iz, kEditTile);
    const auto it = m_edits.find(tileKey(tx, tz));
    if (it == m_edits.end()) return 0.0f;
    return it->second.v[static_cast<std::size_t>((iz - tz * kEditTile) * kEditTile + (ix - tx * kEditTile))];
}

float& Terrain::editRef(int ix, int iz) {
    const int tx = floorDiv(ix, kEditTile), tz = floorDiv(iz, kEditTile);
    EditTile& tile = m_edits[tileKey(tx, tz)];
    return tile.v[static_cast<std::size_t>((iz - tz * kEditTile) * kEditTile + (ix - tx * kEditTile))];
}

// Билинейная выборка поправок: рельеф остаётся гладким между вершинами правок.
float Terrain::editHeightAt(float x, float z) const noexcept {
    const float gx = x / kEditCell, gz = z / kEditCell;
    const int   ix = static_cast<int>(std::floor(gx)), iz = static_cast<int>(std::floor(gz));
    const float fx = gx - static_cast<float>(ix), fz = gz - static_cast<float>(iz);
    const float a = editAt(ix, iz), b = editAt(ix + 1, iz), c = editAt(ix, iz + 1), d = editAt(ix + 1, iz + 1);
    return (a * (1.0f - fx) + b * fx) * (1.0f - fz) + (c * (1.0f - fx) + d * fx) * fz;
}

float Terrain::heightAt(float x, float z) const noexcept {
    const float base = baseHeightAt(x, z);
    return m_edits.empty() ? base : base + editHeightAt(x, z);
}

namespace {
// Гладкий спад кисти: 1 в центре, 0 на краю, плоский у центра и у края.
float falloff(float d, float radius) noexcept {
    const float t = std::clamp(d / radius, 0.0f, 1.0f);
    const float w = 1.0f - t * t;
    return w * w;
}
}  // namespace

std::vector<Terrain::EditTileData> Terrain::exportEdits() const {
    static_assert(kEditTileSize == kEditTile);
    std::vector<EditTileData> out;
    for (const auto& [key, tile] : m_edits) {
        bool any = false;
        for (float v : tile.v) if (v != 0.0f) { any = true; break; }
        if (!any) continue;
        EditTileData d;
        d.tx = static_cast<int>(key >> 32);
        d.tz = static_cast<int>(static_cast<std::int32_t>(key & 0xffffffff));
        d.values.assign(tile.v.begin(), tile.v.end());
        out.push_back(std::move(d));
    }
    // Порядок хэш-таблицы не определён -- сортируем, чтобы файл сохранения был воспроизводим.
    std::sort(out.begin(), out.end(), [](const EditTileData& a, const EditTileData& b) { return a.tx != b.tx ? a.tx < b.tx : a.tz < b.tz; });
    return out;
}

bool Terrain::importEdits(const std::vector<EditTileData>& tiles) {
    for (const auto& d : tiles) {
        if (d.values.size() != static_cast<std::size_t>(kEditTile) * kEditTile) return false;
        EditTile& t = m_edits[tileKey(d.tx, d.tz)];
        for (std::size_t i = 0; i < d.values.size(); ++i) t.v[i] = std::clamp(d.values[i], -kMaxEdit, kMaxEdit);
    }
    return true;
}

Terrain::Rect Terrain::raise(float x, float z, float radius, float amount) {
    radius = std::max(radius, kEditCell);
    const int ix0 = static_cast<int>(std::floor((x - radius) / kEditCell)), ix1 = static_cast<int>(std::ceil((x + radius) / kEditCell));
    const int iz0 = static_cast<int>(std::floor((z - radius) / kEditCell)), iz1 = static_cast<int>(std::ceil((z + radius) / kEditCell));
    for (int iz = iz0; iz <= iz1; ++iz)
        for (int ix = ix0; ix <= ix1; ++ix) {
            const float d = std::hypot(static_cast<float>(ix) * kEditCell - x, static_cast<float>(iz) * kEditCell - z);
            if (d >= radius) continue;
            float& e = editRef(ix, iz);
            e = std::clamp(e + amount * falloff(d, radius), -kMaxEdit, kMaxEdit);
        }
    return {static_cast<float>(ix0 - 1) * kEditCell, static_cast<float>(iz0 - 1) * kEditCell,
            static_cast<float>(ix1 + 1) * kEditCell, static_cast<float>(iz1 + 1) * kEditCell};
}

Terrain::Rect Terrain::flatten(float x, float z, float radius, float target, float rate) {
    radius = std::max(radius, kEditCell);
    const int ix0 = static_cast<int>(std::floor((x - radius) / kEditCell)), ix1 = static_cast<int>(std::ceil((x + radius) / kEditCell));
    const int iz0 = static_cast<int>(std::floor((z - radius) / kEditCell)), iz1 = static_cast<int>(std::ceil((z + radius) / kEditCell));
    for (int iz = iz0; iz <= iz1; ++iz)
        for (int ix = ix0; ix <= ix1; ++ix) {
            const float vx = static_cast<float>(ix) * kEditCell, vz = static_cast<float>(iz) * kEditCell;
            const float d  = std::hypot(vx - x, vz - z);
            if (d >= radius) continue;
            float&      e   = editRef(ix, iz);
            const float cur = baseHeightAt(vx, vz) + e;
            e = std::clamp(e + (target - cur) * std::clamp(rate * falloff(d, radius), 0.0f, 1.0f), -kMaxEdit, kMaxEdit);
        }
    return {static_cast<float>(ix0 - 1) * kEditCell, static_cast<float>(iz0 - 1) * kEditCell,
            static_cast<float>(ix1 + 1) * kEditCell, static_cast<float>(iz1 + 1) * kEditCell};
}

}  // namespace wsim

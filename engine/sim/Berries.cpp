#include "sim/Berries.hpp"

#include <array>
#include <cstring>

namespace wsim {

namespace {

constexpr std::array<BerryDef, kBerryKindCount> kDefs{{
    {"raspberry",  "Малина",             1.00f, 1.00f, 0.00f, {0.80f, 0.08f, 0.14f}, {0.16f, 0.36f, 0.12f}},
    {"blueberry",  "Черника",            0.85f, 1.35f, 0.00f, {0.18f, 0.24f, 0.72f}, {0.14f, 0.32f, 0.21f}},  // мельче, но отрастает быстро
    {"blackberry", "Ежевика",            1.30f, 0.75f, 0.00f, {0.11f, 0.04f, 0.16f}, {0.12f, 0.28f, 0.10f}},  // сытная, растёт медленнее
    {"cloudberry", "Морошка",            1.50f, 0.50f, 0.00f, {0.98f, 0.62f, 0.10f}, {0.32f, 0.40f, 0.15f}},  // самая сытная и самая редкая
    {"tropical",   "Тропическая ягода",  1.15f, 1.20f, 0.00f, {0.58f, 0.16f, 0.80f}, {0.09f, 0.38f, 0.14f}},
    {"cactus",     "Колючий плод",       0.90f, 0.60f, 0.00f, {0.92f, 0.22f, 0.55f}, {0.38f, 0.46f, 0.28f}},  // скудная пустыня
    {"nightshade", "Волчья ягода",       0.60f, 1.00f, 0.90f, {0.20f, 0.05f, 0.30f}, {0.13f, 0.30f, 0.14f}},  // под цвет ежевики, смертельна
    {"baneberry",  "Вороний глаз",       0.70f, 1.00f, 0.50f, {0.94f, 0.94f, 0.88f}, {0.18f, 0.36f, 0.16f}},  // белая, мягче
}};

// Веса видов по биомам (сумма не обязана быть 1: нормируется при выборе).
struct Weights { float w[kBerryKindCount]; };
//                                      Малина Черника Ежевика Морошка Тропич. Колючий Волчья Ворон.
constexpr Weights kWeights[kBiomeCount] = {
    /* Tundra  */ {{0.00f, 0.40f, 0.00f, 0.60f, 0.00f, 0.00f, 0.00f, 0.08f}},
    /* Taiga   */ {{0.15f, 0.60f, 0.00f, 0.25f, 0.00f, 0.00f, 0.10f, 0.10f}},
    /* Plains  */ {{0.50f, 0.20f, 0.30f, 0.00f, 0.00f, 0.00f, 0.05f, 0.08f}},
    /* Forest  */ {{0.30f, 0.30f, 0.40f, 0.00f, 0.00f, 0.00f, 0.13f, 0.10f}},
    /* Desert  */ {{0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 1.00f, 0.00f, 0.00f}},
    /* Savanna */ {{0.00f, 0.00f, 0.00f, 0.00f, 0.30f, 0.70f, 0.00f, 0.04f}},
    /* Jungle  */ {{0.00f, 0.00f, 0.30f, 0.00f, 0.70f, 0.00f, 0.12f, 0.00f}},
};

}  // namespace

const BerryDef& berryDef(BerryKind kind) noexcept { return kDefs[static_cast<std::size_t>(kind)]; }

bool berryFromId(std::string_view id, BerryKind& out) noexcept {
    for (int i = 0; i < kBerryKindCount; ++i)
        if (id == kDefs[static_cast<std::size_t>(i)].id) { out = static_cast<BerryKind>(i); return true; }
    return false;
}

BerryKind pickBerryKind(Biome biome, float u) noexcept {
    const Weights& w = kWeights[static_cast<int>(biome)];
    float total = 0.0f;
    for (float x : w.w) total += x;
    float t = u * total;
    int   last = 0;
    for (int i = 0; i < kBerryKindCount; ++i) {
        if (w.w[i] <= 0.0f) continue;
        last = i;
        if (t < w.w[i]) return static_cast<BerryKind>(i);
        t -= w.w[i];
    }
    return static_cast<BerryKind>(last);  // u == 1 или накопленная погрешность: последний подходящий
}

bool berryGrowsIn(BerryKind kind, Biome biome) noexcept {
    return kWeights[static_cast<int>(biome)].w[static_cast<int>(kind)] > 0.0f;
}

float bushDensity(Biome biome) noexcept {
    switch (biome) {
        case Biome::Jungle:  return 1.25f;
        case Biome::Forest:  return 1.0f;
        case Biome::Plains:  return 0.85f;
        case Biome::Taiga:   return 0.7f;
        case Biome::Savanna: return 0.5f;
        case Biome::Tundra:  return 0.35f;
        case Biome::Desert:  return 0.25f;
    }
    return 1.0f;
}

}  // namespace wsim

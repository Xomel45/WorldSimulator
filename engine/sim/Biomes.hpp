#pragma once

#include <cstdint>

namespace wsim {

// Климат точки: температура и влажность в 0..1. Чистые данные, без зависимостей -- биом из них выбирает
// classify() (таблица Уиттекера в упрощённом виде), а шейдер рельефа получает те же два числа и
// плавно смешивает палитры, поэтому на границах биомов нет резких швов.
struct Climate {
    float temperature = 0.5f;  // 0 -- арктика, 1 -- жара
    float moisture    = 0.5f;  // 0 -- сушь, 1 -- ливни
};

enum class Biome : std::uint8_t { Tundra, Taiga, Plains, Forest, Desert, Savanna, Jungle };
inline constexpr int kBiomeCount = 7;

// Высота остужает: на каждую единицу выше kLapseStart температура падает на kLapsePerUnit. Так горы сами
// покрываются снегом, а на склонах климат ниже по шкале (лес -> тайга -> тундра).
inline constexpr float kLapseStart   = 3.0f;
inline constexpr float kLapsePerUnit = 0.035f;

[[nodiscard]] inline Climate withAltitude(Climate c, float height) noexcept {
    if (height > kLapseStart) c.temperature -= (height - kLapseStart) * kLapsePerUnit;
    if (c.temperature < 0.0f) c.temperature = 0.0f;
    return c;
}

// Границы: холод < 0.24 <= умеренно < 0.68 <= жарко; сухо/влажно -- свои пороги в каждом поясе.
[[nodiscard]] inline Biome classify(const Climate& c) noexcept {
    if (c.temperature < 0.24f) return c.moisture < 0.40f ? Biome::Tundra : Biome::Taiga;
    if (c.temperature < 0.68f) return c.moisture < 0.55f ? Biome::Plains : Biome::Forest;
    if (c.moisture < 0.35f) return Biome::Desert;
    return c.moisture < 0.60f ? Biome::Savanna : Biome::Jungle;
}

[[nodiscard]] inline const char* biomeName(Biome b) noexcept {
    switch (b) {
        case Biome::Tundra:  return "Тундра";
        case Biome::Taiga:   return "Тайга";
        case Biome::Plains:  return "Равнины";
        case Biome::Forest:  return "Лес";
        case Biome::Desert:  return "Пустыня";
        case Biome::Savanna: return "Саванна";
        case Biome::Jungle:  return "Джунгли";
    }
    return "?";
}

}  // namespace wsim

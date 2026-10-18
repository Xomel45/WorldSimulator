#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace wsim {

// Постройки существ в мире (не путать с Building -- дома бога-декорации). Мосты живут в Terrain (проходимость воды), здесь --
// баррикады (останавливают зверей) и ловушки (ранят и обездвиживают хищника). Данные без логики: решают World и Fauna.
enum class StructureKind : std::uint8_t { Barricade = 1, Trap = 2, Stockpile = 3, Hut = 4 };
inline constexpr float kHutRadius = 1.5f;   // хижина (круглый в плане) не пускает зверей, спящие рядом под защитой стен
inline constexpr float kStockpileCapacity = 40.0f;   // брёвен на складе

inline constexpr float kBarricadeLength = 3.6f;   // вдоль оси yaw
inline constexpr float kBarricadeHalfWidth = 0.55f;
inline constexpr float kTrapRadius = 1.1f;

struct Structure {
    StructureKind kind = StructureKind::Barricade;
    float         x = 0.0f, z = 0.0f;
    float         yaw = 0.0f;   // радианы: ось баррикады (cos -> x, sin -> z)
    float         hp = 1.0f;    // ловушка при срабатывании -> 0 и исчезает
    float         stock = 0.0f; // склад: сколько брёвен лежит
};

// Преграждает ли баррикада (или хижина) зверям точку (x, z) с запасом margin (расстояние до отрезка оси).
[[nodiscard]] inline bool barricadeBlocks(const Structure& s, float x, float z, float margin) {
    if (s.kind == StructureKind::Hut) {
        const float dx = x - s.x, dz = z - s.z, r = kHutRadius + margin;
        return dx * dx + dz * dz < r * r;
    }
    if (s.kind != StructureKind::Barricade) return false;
    const float ax = std::cos(s.yaw), az = std::sin(s.yaw);
    const float px = x - s.x, pz = z - s.z;
    const float along = std::clamp(px * ax + pz * az, -kBarricadeLength * 0.5f, kBarricadeLength * 0.5f);
    const float dx = px - ax * along, dz = pz - az * along;
    return dx * dx + dz * dz < (kBarricadeHalfWidth + margin) * (kBarricadeHalfWidth + margin);
}
[[nodiscard]] inline bool blockedByBarricade(std::span<const Structure> all, float x, float z, float margin) {
    for (const Structure& s : all)
        if (barricadeBlocks(s, x, z, margin)) return true;
    return false;
}

}  // namespace wsim

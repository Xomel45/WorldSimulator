#pragma once

#include "sim/Biomes.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>

namespace wsim {

// Шесть видов ягод. Каждому биому подходят свои: в тундре морошка и черника, в лесу малина, ежевика и черника,
// в джунглях тропические ягоды, в пустыне -- плоды колючих кустов. Виды различаются питательностью, скоростью
// отрастания и цветом ягод и листвы (для картинки). Ядовитых пока нет: существа выбирают куст по расстоянию и
// не отличают виды, так что яд превратился бы просто в случайную смерть.
enum class BerryKind : std::uint8_t { Raspberry, Blueberry, Blackberry, Cloudberry, Tropical, Cactus };
inline constexpr int kBerryKindCount = 6;

struct BerryDef {
    const char* id;          // для модов: "raspberry"...
    const char* name;        // Русское название
    float       nutrition;   // во сколько раз сытнее малины (малина = 1)
    float       regrow;      // во сколько раз быстрее малины отрастает куст
    glm::vec3   berryColor;  // цвет ягод (как в draw-списке граней куста)
    glm::vec3   leafColor;   // цвет листвы
};

[[nodiscard]] const BerryDef& berryDef(BerryKind kind) noexcept;
[[nodiscard]] inline const char* berryName(BerryKind kind) noexcept { return berryDef(kind).name; }
// Вид по id ("blueberry"); false -- неизвестный.
[[nodiscard]] bool berryFromId(std::string_view id, BerryKind& out) noexcept;

// Какие ягоды растут в биоме: u в [0,1) выбирает вид по весам биома. Всегда возвращает вид, подходящий биому.
[[nodiscard]] BerryKind pickBerryKind(Biome biome, float u) noexcept;
// Подходит ли вид биому (вес > 0) -- для тестов и проверок.
[[nodiscard]] bool berryGrowsIn(BerryKind kind, Biome biome) noexcept;
// Во сколько раз кустов в биоме больше или меньше среднего (джунгли и лес богаче, пустыня и тундра беднее).
[[nodiscard]] float bushDensity(Biome biome) noexcept;

}  // namespace wsim

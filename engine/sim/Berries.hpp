#pragma once

#include "sim/Biomes.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>

namespace wsim {

// Одиннадцать видов ягод. Шесть съедобных (малина, черника, ежевика, морошка, тропическая, колючий плод) и пять
// ядовитых с разным действием: смерть (волчья ягода), рвота + понос (вороний глаз), понос (крушина), рвота (жимолость),
// галлюцинации (белена). Каждому биому подходят свои: в тундре морошка и черника, в лесу малина, ежевика и черника,
// в джунглях тропические, в пустыне -- плоды колючих кустов; ядовитых везде мало.
// Виды различаются питательностью, скоростью отрастания, цветом ягод и листвы и действием на едока.
enum class BerryKind : std::uint8_t { Raspberry, Blueberry, Blackberry, Cloudberry, Tropical, Cactus, Nightshade, Baneberry, Buckthorn, Honeysuckle, Henbane };
inline constexpr int kBerryKindCount = 11;

// Внешность: существа различают ягоды по ней, а не по виду растения. Подделки -- в одной группе: ежевика и волчья ягода
// ("тёмные"), черника и крушина ("синие"), малина и жимолость ("красные"), морошка и белена ("оранжевые"); съев одну,
// существо составляет мнение обо всей группе (и может отвернуться от хорошей ягоды из-за плохой, как в жизни).
enum class BerryLook : std::uint8_t { Red, Blue, Dark, Orange, Violet, Pink, White };
inline constexpr int kBerryLookCount = 7;

struct BerryDef {
    const char* id;          // для модов: "raspberry"...
    const char* name;        // Русское название
    float       nutrition;   // во сколько раз сытнее малины (малина = 1)
    float       regrow;      // во сколько раз быстрее малины отрастает куст
    // Действие на едока: доли "полной дозы" на единицу съеденного (0 -- нет). См. Creature::ingestBerry.
    float       lethal;         // яд, бьющий по здоровью (смерть при большой дозе)
    float       diarrhea;       // понос: еда проскакивает, силы тают, едва идёт
    float       vomit;          // рвота: тошнота, потом приступ -- опустошает желудок (и выводит яд)
    float       hallucination;  // галлюцинации: мерещатся угрозы, мечется, зовёт на помощь без причины
    glm::vec3   berryColor;  // цвет ягод (как в draw-списке граней куста)
    glm::vec3   leafColor;   // цвет листвы
    BerryLook   look;

    // Сколько "плохого" в ягоде с точки зрения вкуса (в единицах сытности): смерть хуже всего, галлюцинации -- мягче.
    [[nodiscard]] float harm() const noexcept { return 2.0f * lethal + 0.9f * diarrhea + 1.0f * vomit + 0.6f * hallucination; }
};

[[nodiscard]] const BerryDef& berryDef(BerryKind kind) noexcept;
[[nodiscard]] inline const char* berryName(BerryKind kind) noexcept { return berryDef(kind).name; }
[[nodiscard]] inline bool isPoisonous(BerryKind kind) noexcept { return berryDef(kind).harm() > 0.0f; }
[[nodiscard]] inline BerryLook lookOf(BerryKind kind) noexcept { return berryDef(kind).look; }
[[nodiscard]] const char* lookName(BerryLook look) noexcept;  // "красные", "тёмные"...
// Вид по id ("blueberry"); false -- неизвестный.
[[nodiscard]] bool berryFromId(std::string_view id, BerryKind& out) noexcept;

// Какие ягоды растут в биоме: u в [0,1) выбирает вид по весам биома. Всегда возвращает вид, подходящий биому.
[[nodiscard]] BerryKind pickBerryKind(Biome biome, float u) noexcept;
// Подходит ли вид биому (вес > 0) -- для тестов и проверок.
[[nodiscard]] bool berryGrowsIn(BerryKind kind, Biome biome) noexcept;
// Во сколько раз кустов в биоме больше или меньше среднего (джунгли и лес богаче, пустыня и тундра беднее).
[[nodiscard]] float bushDensity(Biome biome) noexcept;

}  // namespace wsim

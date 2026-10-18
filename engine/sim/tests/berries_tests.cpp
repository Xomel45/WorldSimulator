#include "sim/Berries.hpp"
#include "sim/World.hpp"

#include "test_util.hpp"

#include <cmath>
#include <span>
#include <cstdio>
#include <set>
#include <string>

using namespace wsim;

namespace {
constexpr float kDt = 1.0f / 60.0f;
}

int main() {
    // Таблица видов: id уникальны и находятся обратно, малина -- эталон (сытность и отрастание = 1), цвета различаются.
    {
        std::set<std::string> ids;
        for (int k = 0; k < kBerryKindCount; ++k) {
            const BerryDef& d = berryDef(static_cast<BerryKind>(k));
            WSIM_CHECK(ids.insert(d.id).second && d.name[0] != '\0');
            BerryKind back{};
            WSIM_CHECK(berryFromId(d.id, back) && back == static_cast<BerryKind>(k));
            WSIM_CHECK(d.nutrition > 0.3f && d.nutrition < 3.0f && d.regrow > 0.3f && d.regrow < 3.0f);
        }
        const BerryDef& rasp = berryDef(BerryKind::Raspberry);
        WSIM_CHECK(rasp.nutrition == 1.0f && rasp.regrow == 1.0f);
        BerryKind dummy{};
        WSIM_CHECK(!berryFromId("durian", dummy) && !berryFromId("", dummy));
        WSIM_CHECK(berryDef(BerryKind::Cloudberry).nutrition > berryDef(BerryKind::Blueberry).nutrition);  // морошка сытнее
        WSIM_CHECK(berryDef(BerryKind::Blueberry).regrow > berryDef(BerryKind::Cloudberry).regrow);        // черника отрастает быстрее
    }

    // Выбор вида: всегда подходящий биому, по всей шкале u; в каждом биоме есть хотя бы один вид.
    {
        for (int b = 0; b < kBiomeCount; ++b) {
            std::set<BerryKind> seen;
            for (int i = 0; i <= 200; ++i) {
                const BerryKind k = pickBerryKind(static_cast<Biome>(b), static_cast<float>(i) / 200.0f);
                WSIM_CHECK(berryGrowsIn(k, static_cast<Biome>(b)));
                seen.insert(k);
            }
            WSIM_CHECK(!seen.empty());
            WSIM_CHECK(bushDensity(static_cast<Biome>(b)) > 0.0f);
        }
        WSIM_CHECK(pickBerryKind(Biome::Desert, 0.5f) == BerryKind::Cactus);
        WSIM_CHECK(pickBerryKind(Biome::Jungle, 0.0f) != pickBerryKind(Biome::Tundra, 0.0f));
        WSIM_CHECK(bushDensity(Biome::Jungle) > bushDensity(Biome::Forest) && bushDensity(Biome::Forest) > bushDensity(Biome::Desert));
    }

    // Процедурные кусты: чистая функция сида и ячейки -- воспроизводимы, на суше, виды по биому, ядовитых мало, плотность разумна,
    // правки рельефа не сбивают номера кандидатов (по ним мир помнит состояние спящих ячеек).
    {
        std::size_t bushes = 0, bad = 0, cells = 0;
        for (std::uint32_t seed : {5u, 11u, 29u, 77u, 1234u, 9999u}) {
            Terrain t(World::kWorldHalfSize, seed), t2(World::kWorldHalfSize, seed), other(World::kWorldHalfSize, seed + 1);
            bool differs = false;
            for (std::int32_t cz = -25; cz < 25; ++cz)
                for (std::int32_t cx = -25; cx < 25; ++cx) {   // 900x900 единиц
                    const auto a1 = proceduralBushes(t, cx, cz), a2 = proceduralBushes(t2, cx, cz), a3 = proceduralBushes(other, cx, cz);
                    ++cells;
                    WSIM_CHECK(a1.size() == a2.size() && a1.size() <= 3);
                    differs |= a1.size() != a3.size() || (!a1.empty() && !a3.empty() && a1[0].x != a3[0].x);
                    std::set<int> locals;
                    for (std::size_t i = 0; i < a1.size(); ++i) {
                        WSIM_CHECK(a1[i].x == a2[i].x && a1[i].z == a2[i].z && a1[i].kind == a2[i].kind && a1[i].local == a2[i].local);
                        WSIM_CHECK(!t.isUnderwater(a1[i].x, a1[i].z) && t.heightAt(a1[i].x, a1[i].z) > 0.4f);
                        WSIM_CHECK(a1[i].local >= 0 && a1[i].local < 3 && locals.insert(a1[i].local).second);
                        // виды по биому (у границы биомов куст пятна может оказаться по другую сторону)
                        bool fits = false;
                        for (float dx : {0.0f, -5.0f, 5.0f}) for (float dz : {0.0f, -5.0f, 5.0f})
                            fits |= berryGrowsIn(a1[i].kind, classify(t.climateAt(a1[i].x + dx, a1[i].z + dz)));
                        WSIM_CHECK(fits);
                        ++bushes;
                        bad += isPoisonous(a1[i].kind);
                    }
                }
            WSIM_CHECK(differs);   // другой сид -- другая расстановка
        }
        const double perCell = static_cast<double>(bushes) / static_cast<double>(cells);
        WSIM_CHECK(perCell > 0.12 && perCell < 1.0);          // ~0.3-0.5 куста на ячейку 18x18 (с учётом воды и пустынь)
        WSIM_CHECK(static_cast<double>(bad) < 0.15 * static_cast<double>(bushes));   // ядовитых мало
        // Правка рельефа: затопили первый куст ячейки -- его нет, а остальные сохранили свои номера.
        Terrain t(World::kWorldHalfSize, 29);
        bool tested = false;
        for (std::int32_t cz = -8; cz < 8 && !tested; ++cz)
            for (std::int32_t cx = -8; cx < 8 && !tested; ++cx) {
                const auto before = proceduralBushes(t, cx, cz);
                if (before.size() < 2) continue;
                Terrain edited(World::kWorldHalfSize, 29);
                for (int i = 0; i < 12; ++i) edited.raise(before[0].x, before[0].z, 2.0f, -3.0f);
                const auto after = proceduralBushes(edited, cx, cz);
                if (after.size() >= before.size()) continue;
                for (const BushSpec& b : after)
                    for (const BushSpec& old : before)
                        if (b.local == old.local) WSIM_CHECK(b.x == old.x && b.z == old.z && b.kind == old.kind);
                tested = true;
            }
        WSIM_CHECK(tested);
    }

    // Мир без границ: кусты подгружаются там, где есть существа/камера, спящие ячейки помнят съеденное, стёртое и посаженное,
    // а потерянное отрастает за время сна.
    {
        Terrain t(World::kWorldHalfSize, 29);
        World   w(29);
        w.growBerryBushes(t);
        WSIM_CHECK(w.foodCount() > 8 && w.loadedBushCells() > 100);
        {   // без существ-якорей: только камера решает, где мир "живой"
            WorldState empty = w.exportState();
            empty.population.clear();
            WSIM_CHECK(w.importState(empty));
        }
        // Старт: кусты стартовой области на месте; потом камера уходит -- они засыпают, у камеры просыпаются новые.
        glm::vec2 here{0.0f, 0.0f}, far{700.0f, -650.0f};
        w.setFocus(std::span<const glm::vec2>(&here, 1));
        for (int i = 0; i < 60; ++i) w.tick(kDt, &t);
        const std::size_t atStart = w.foodCount();
        WSIM_CHECK(atStart > 8);
        // Помечаем реальный куст: съедено до 0.3 и один стёрт.
        std::size_t victim = 0, erased = 1;
        for (std::size_t i = 0; i < w.foodCount(); ++i) if (w.food()[i].local >= 0 && std::fabs(w.food()[i].position.x) < 40.0f) { victim = i; break; }
        for (std::size_t i = 0; i < w.foodCount(); ++i) if (i != victim && w.food()[i].local >= 0 && std::fabs(w.food()[i].position.x) < 40.0f) { erased = i; break; }
        WSIM_CHECK(victim != erased);
        WorldState st = w.exportState();
        const glm::vec3 victimPos = st.food[victim].position, erasedPos = st.food[erased].position;
        const BerryKind victimKind = st.food[victim].kind;
        st.food[victim].amount = 0.3f;
        WSIM_CHECK(w.importState(st));
        WSIM_CHECK(w.removeBushes(erasedPos.x, erasedPos.z, 0.05f) == 1);
        // Посаженный куст тоже должен пережить сон.
        glm::vec3 plantedAt{-60.0f, 0.0f, -60.0f};
        for (float dx = 0; dx < 60.0f; dx += 3.0f)
            if (t.heightAt(-60.0f + dx, -60.0f) > 1.0f && w.plantBush(-60.0f + dx, -60.0f, BerryKind::Cloudberry)) { plantedAt = {-60.0f + dx, 0.0f, -60.0f}; break; }
        const std::size_t withPlanted = w.foodCount();
        // Камера далеко: старая область засыпает.
        w.setFocus(std::span<const glm::vec2>(&far, 1));
        for (int i = 0; i < 60 * 4; ++i) w.tick(kDt, &t);
        bool anyNearStart = false, anyNearFar = false;
        for (const FoodSource& f : w.food()) {
            anyNearStart |= std::hypot(f.position.x, f.position.z) < 80.0f;
            anyNearFar   |= std::hypot(f.position.x - far.x, f.position.z - far.y) < 90.0f;
        }
        WSIM_CHECK(!anyNearStart && anyNearFar);                       // здесь спят, там проснулись
        WSIM_CHECK(w.foodCount() < withPlanted + 400 && w.foodCount() > 0);
        // Прошло время; возвращаемся: куст отрос по формуле, стёртый не вернулся, посаженный на месте.
        for (int i = 0; i < 60 * 40; ++i) w.tick(kDt, &t);             // ~40 с сна
        w.setFocus(std::span<const glm::vec2>(&here, 1));
        for (int i = 0; i < 60; ++i) w.tick(kDt, &t);
        bool victimFound = false, erasedFound = false, plantedFound = false;
        for (const FoodSource& f : w.food()) {
            if (std::hypot(f.position.x - victimPos.x, f.position.z - victimPos.z) < 0.01f) {
                victimFound = true;
                const float k = 0.75f * 0.075f * berryDef(victimKind).regrow;
                const float expected = std::min(1.0f, (0.3f + 1.0f / 3.0f) * std::exp(k * 44.0f) - 1.0f / 3.0f);   // сон ~44 с (4 + 40) + пара тиков
                WSIM_CHECK(f.amount > 0.3f && std::fabs(f.amount - expected) < 0.06f);
            }
            erasedFound  |= std::hypot(f.position.x - erasedPos.x, f.position.z - erasedPos.z) < 0.01f;
            plantedFound |= std::hypot(f.position.x - plantedAt.x, f.position.z - plantedAt.z) < 0.01f && f.kind == BerryKind::Cloudberry && f.local < 0;
        }
        WSIM_CHECK(victimFound && !erasedFound && plantedFound);
        // Сохранение: состояние спящих ячеек (записи) и происхождение кустов переживают экспорт/импорт.
        w.setFocus(std::span<const glm::vec2>(&far, 1));
        for (int i = 0; i < 60 * 3; ++i) w.tick(kDt, &t);
        const WorldState s1 = w.exportState();
        WSIM_CHECK(s1.streaming && !s1.cellRecords.empty() && !s1.loadedCells.empty());
        World w2(1);
        WSIM_CHECK(w2.importState(s1));
        const WorldState s2 = w2.exportState();
        WSIM_CHECK(s2.cellRecords.size() == s1.cellRecords.size() && s2.loadedCells == s1.loadedCells && s2.food.size() == s1.food.size() && s2.time == s1.time);
        // Старый формат: кусты списком без ячеек -- читаются как посаженные, а стартовая область не обрастает копиями процедурных.
        WorldState legacy;
        legacy.rng = s1.rng;
        legacy.food = {FoodSource{{10.0f, 0.0f, 10.0f}, 0.5f, BerryKind::Blueberry}};
        World w3(2);
        WSIM_CHECK(w3.importState(legacy));
        w3.setFocus(std::span<const glm::vec2>(&here, 1));
        for (int i = 0; i < 120; ++i) w3.tick(kDt, &t);
        WSIM_CHECK(w3.foodCount() >= 1 && w3.food()[0].local < 0);
        std::size_t inRegion = 0;
        for (const FoodSource& f : w3.food()) inRegion += std::fabs(f.position.x) < 90.0f && std::fabs(f.position.z) < 90.0f;
        WSIM_CHECK(inRegion == 1);   // только старый куст -- процедурные в "загруженной" стартовой области не рождаются
    }

    // Существа не ограничены стартовой областью (в потоковом мире), а в обычном (без рельефа) границы мягко держат.
    {
        Terrain t(World::kWorldHalfSize, 29);
        auto runner = [&](bool streaming) {
            Rng rng(3);
            Creature c;
            c.genome = Genome::random(rng);
            c.position = {96.0f, 0.0f, 0.0f};
            c.age = c.lifespan() * 0.3f;
            c.id = 1;
            c.needs.hunger = 0.1f;
            c.wanderHeading = 0.0f;
            c.exploreTimer = 120.0f;   // идёт прямо на +x
            World w(5);
            WorldState st = w.exportState();
            st.population.clear();
            st.population.push_back({c, BrainOutputs{}});
            st.food.clear();
            st.nextId = 100;
            st.streaming = streaming;
            WSIM_CHECK(w.importState(st));
            float maxX = 0.0f;
            for (int i = 0; i < 60 * 60; ++i) {
                w.tick(kDt, streaming ? &t : nullptr);
                if (!w.population().empty()) maxX = std::max(maxX, w.population()[0].creature.position.x);
            }
            return maxX;
        };
        const float free = runner(true), bounded = runner(false);
        WSIM_CHECK(free > 125.0f);          // ушло далеко за стартовую область
        WSIM_CHECK(bounded < 115.0f);       // в мире с границами мягко удерживается
    }

    // Питательность и отрастание зависят от вида: два опустошённых куста разных видов за одно время отрастают по-разному;
    // одно и то же существо сытеет от морошки сильнее, чем от черники.
    {
        Terrain t(World::kWorldHalfSize, 9);
        auto build = [&](BerryKind kind) {
            World w(9);
            WorldState st = w.exportState();
            st.population.clear();
            st.food.clear();
            st.food.push_back(FoodSource{{10.0f, 0.0f, 10.0f}, 0.2f, kind});
            WSIM_CHECK(w.importState(st));
            for (int i = 0; i < 600; ++i) w.tick(kDt, nullptr);  // 10 с
            return w.food()[0].amount;
        };
        const float blue = build(BerryKind::Blueberry), rasp = build(BerryKind::Raspberry), cloud = build(BerryKind::Cloudberry);
        WSIM_CHECK(blue > rasp && rasp > cloud);
        // da/dt = 0.075 * regrow * (0.25 + 0.75 a): за 10 с из 0.2 выходит 0.81 / 0.60 / 0.37 (черника / малина / морошка)
        WSIM_CHECK(std::fabs(blue - 0.806f) < 0.02f && std::fabs(rasp - 0.603f) < 0.02f && std::fabs(cloud - 0.373f) < 0.02f);
        // Объеденный куст отрастает с задержкой: из нуля еду (0.12) даёт не сразу, а через ~7.5 с.
        {
            World w(9);
            WorldState st = w.exportState();
            st.population.clear();
            st.food = {FoodSource{{10.0f, 0.0f, 10.0f}, 0.0f, BerryKind::Raspberry}};
            WSIM_CHECK(w.importState(st));
            int edible = -1, full = -1;
            for (int i = 0; i < 60 * 200 && full < 0; ++i) {
                w.tick(kDt, nullptr);
                if (edible < 0 && w.food()[0].amount >= 0.12f) edible = i;
                if (w.food()[0].amount >= 0.999f) full = i;
            }
            WSIM_CHECK(edible > 60 * 4 && edible < 60 * 9);   // ~5.5 с до первой еды
            WSIM_CHECK(full > 60 * 20 && full < 60 * 36);      // ~25 с до полного
        }

        auto hungerAfterMeal = [&](BerryKind kind) {
            World w(9);
            WorldState st = w.exportState();
            st.population.resize(1);
            st.food.clear();
            Creature& c = st.population[0].creature;
            c.position  = {10.0f, 0.0f, 10.0f};
            c.needs.hunger = 0.9f;
            st.food.push_back(FoodSource{{10.0f, 0.0f, 10.0f}, 1.0f, kind});
            WSIM_CHECK(w.importState(st));
            for (int i = 0; i < 60; ++i) w.tick(kDt, nullptr);  // 1 с рядом с кустом
            return w.population().empty() ? 1.0f : w.population()[0].creature.needs.hunger;
        };
        WSIM_CHECK(hungerAfterMeal(BerryKind::Cloudberry) < hungerAfterMeal(BerryKind::Blueberry));
    }

    // plantBush: потолок kMaxFood, интервал между кустами, удаление и поиск ближайшего.
    {
        World w(3);
        const std::size_t base = w.foodCount();
        WSIM_CHECK(w.plantBush(50.0f, 50.0f, BerryKind::Blackberry));
        WSIM_CHECK(!w.plantBush(50.5f, 50.0f, BerryKind::Blackberry));
        std::size_t idx = 0;
        WSIM_CHECK(w.nearestBush(50.4f, 50.2f, 2.0f, idx) && w.food()[idx].kind == BerryKind::Blackberry);
        WSIM_CHECK(!w.nearestBush(50.0f, 70.0f, 2.0f, idx));
        WSIM_CHECK(w.removeBushes(50.0f, 50.0f, 1.0f) == 1 && w.foodCount() == base);
        for (int i = 0; w.foodCount() < World::kMaxFood && i < 10000; ++i)
            w.plantBush(-95.0f + static_cast<float>(i % 60) * 3.0f, -95.0f + static_cast<float>(i / 60) * 3.0f, BerryKind::Raspberry);
        WSIM_CHECK(w.foodCount() == World::kMaxFood && !w.plantBush(0.0f, 99.0f, BerryKind::Raspberry));
    }

    std::puts("berries_tests: OK");
    return 0;
}

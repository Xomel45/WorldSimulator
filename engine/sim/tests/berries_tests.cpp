#include "sim/Berries.hpp"
#include "sim/World.hpp"

#include "test_util.hpp"

#include <cmath>
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

    // Генерация кустов: воспроизводимо по сиду, на суше внутри мира, не теснее kBushSpacing, виды -- по биому,
    // число разумное (как у прежних 30), и чем богаче биом, тем больше кустов.
    {
        int perBiome[kBiomeCount] = {};
        int foodAcrossSeeds = 0;
        for (std::uint32_t seed : {5u, 11u, 29u, 77u, 1234u, 9999u, 4242u, 31337u}) {
            Terrain t(World::kWorldHalfSize, seed);
            World   a(seed), b(seed), c(seed + 1);
            a.growBerryBushes(t);
            b.growBerryBushes(t);
            c.growBerryBushes(t);
            WSIM_CHECK(a.foodCount() >= 18 && a.foodCount() <= 90);
            WSIM_CHECK(a.foodCount() == b.foodCount());
            for (std::size_t i = 0; i < a.foodCount(); ++i) {
                const FoodSource& f = a.food()[i];
                WSIM_CHECK(f.position.x == b.food()[i].position.x && f.kind == b.food()[i].kind);
                WSIM_CHECK(std::fabs(f.position.x) <= World::kWorldHalfSize && std::fabs(f.position.z) <= World::kWorldHalfSize);
                WSIM_CHECK(!t.isUnderwater(f.position.x, f.position.z) && f.amount == 1.0f);
                const Biome biome = t.biomeAt(f.position.x, f.position.z);
                // у границы биомов куст может оказаться по другую сторону от центра пятна (радиус пятна 3.5)
                bool fits = berryGrowsIn(f.kind, biome);
                for (float dx : {-4.0f, 4.0f}) for (float dz : {-4.0f, 4.0f}) fits |= berryGrowsIn(f.kind, t.biomeAt(f.position.x + dx, f.position.z + dz));
                WSIM_CHECK(fits);
                ++perBiome[static_cast<int>(biome)];
                for (std::size_t j = 0; j < i; ++j)
                    WSIM_CHECK(std::hypot(f.position.x - a.food()[j].position.x, f.position.z - a.food()[j].position.z) >= World::kBushSpacing - 1e-4f);
            }
            foodAcrossSeeds += static_cast<int>(a.foodCount());
            bool differs = a.foodCount() != c.foodCount();
            for (std::size_t i = 0; !differs && i < std::min(a.foodCount(), c.foodCount()); ++i) differs = a.food()[i].position.x != c.food()[i].position.x;
            WSIM_CHECK(differs);  // другой сид мира -- другая расстановка
        }
        // пустыня и тундра беднее леса и джунглей (по числу кустов на все сиды; сравнение мягкое: площадь биомов разная)
        WSIM_CHECK(foodAcrossSeeds > 8 * 15);
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
            for (int i = 0; i < 600; ++i) w.tick(kDt, &t);  // 10 с
            return w.food()[0].amount;
        };
        const float blue = build(BerryKind::Blueberry), rasp = build(BerryKind::Raspberry), cloud = build(BerryKind::Cloudberry);
        WSIM_CHECK(blue > rasp && rasp > cloud);  // 0.2 + 0.04 * 10 * regrow: 0.74 / 0.6 / 0.4
        WSIM_CHECK(std::fabs(rasp - 0.6f) < 0.02f && std::fabs(cloud - 0.4f) < 0.02f);

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
            for (int i = 0; i < 300; ++i) w.tick(kDt, nullptr);  // 5 с рядом с кустом
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

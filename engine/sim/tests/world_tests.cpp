#include "sim/Terrain.hpp"
#include "sim/World.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace wsim;

namespace {
// ~50 симулированных секунд, dt как в реальном игровом цикле -- надёжно
// меньше 300с минимального lifespan(), никто не должен умереть от старости.
constexpr int   kTicks = 3000;
constexpr float kDt    = 1.0f / 60.0f;

void runTicks(World& world) {
    for (int i = 0; i < kTicks; ++i)
        world.tick(kDt);
}
}  // namespace

int main() {
    World world(/*seed=*/12345);
    WSIM_CHECK(world.populationCount() == World::kInitialPopulation);
    const std::size_t initialFood = world.foodCount();

    runTicks(world);

    WSIM_CHECK(world.populationCount() > 0);                     // никто не должен был вымереть за ~50с
    WSIM_CHECK(world.populationCount() <= World::kMaxPopulation);  // потолок реально держит
    WSIM_CHECK(world.foodCount() == initialFood);  // источники не исчезают/не появляются, только истощаются/растут

    // Позиции конечны (ловит "улетевшую в NaN/Inf" физику), еда в [0, cap].
    for (const auto& inh : world.population()) {
        WSIM_CHECK(std::isfinite(inh.creature.position.x));
        WSIM_CHECK(std::isfinite(inh.creature.position.y));
        WSIM_CHECK(std::isfinite(inh.creature.position.z));
    }
    for (const auto& src : world.food()) {
        WSIM_CHECK(std::isfinite(src.amount));
        WSIM_CHECK(src.amount >= 0.0f && src.amount <= 1.0f);
    }

    // id уникальны и ненулевые (включая, если размножение случилось, детей).
    std::vector<std::uint64_t> ids;
    ids.reserve(world.populationCount());
    for (const auto& inh : world.population()) {
        WSIM_CHECK(inh.creature.id != 0);
        ids.push_back(inh.creature.id);
    }
    std::sort(ids.begin(), ids.end());
    WSIM_CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    // Детерминизм: тот же сид -> та же траектория популяции.
    World worldB(12345);
    runTicks(worldB);
    WSIM_CHECK(worldB.populationCount() == world.populationCount());

    // Физика рельефа: существа не заходят в воду и стоят на поверхности земли.
    for (std::uint32_t seed : {3u, 11u, 29u}) {
        Terrain terrain(World::kWorldHalfSize, seed);
        World   w(seed);
        w.settleOnLand(terrain);
        for (const auto& in : w.population()) WSIM_CHECK(!terrain.isUnderwater(in.creature.position.x, in.creature.position.z));
        for (const auto& f : w.food()) WSIM_CHECK(!terrain.isUnderwater(f.position.x, f.position.z));

        for (int i = 0; i < 1800; ++i) {
            w.tick(1.0f / 60.0f, &terrain);
            for (const auto& in : w.population()) {
                const auto& p = in.creature.position;
                WSIM_CHECK(!terrain.isUnderwater(p.x, p.z));
                WSIM_CHECK(std::fabs(p.y - terrain.heightAt(p.x, p.z)) < 1.0e-3f);
            }
        }
        WSIM_CHECK(w.populationCount() > 0);
    }

    std::puts("world_tests: OK");
    return 0;
}

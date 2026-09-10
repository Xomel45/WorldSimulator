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

    std::puts("world_tests: OK");
    return 0;
}

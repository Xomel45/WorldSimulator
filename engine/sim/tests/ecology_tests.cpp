// Регрессия баланса: короткая "жизнь" с рельефом (всё включено: ягоды, звери, жажда, сон, сезоны) на двух сидах.
// Не требует хорошего результата, а ловит провалы: вымирание, остановку рождений, взрыв до потолка.
#include "sim/Terrain.hpp"
#include "sim/World.hpp"

#include "test_util.hpp"

#include <cstdio>

using namespace wsim;

int main() {
    constexpr float kDt = 1.0f / 60.0f;
    for (std::uint32_t seed : {11u, 1234u}) {
        Terrain t(World::kWorldHalfSize, seed);
        World   w(seed);
        w.settleOnLand(t);
        w.growBerryBushes(t);
        w.growWildlife(t);
        w.formTribes(t);   // как в игре: три племени
        double popSum = 0.0;
        long   samples = 0;
        std::size_t peak = 0;
        for (int i = 0; i < 60 * 60 * 8; ++i) {   // 8 минут
            const glm::vec2 focus{0.0f, 0.0f};
            w.setFocus(std::span<const glm::vec2>(&focus, 1));
            w.tick(kDt, &t);
            (void)w.takeDirty();
            if (i % 60 == 0) { popSum += static_cast<double>(w.populationCount()); ++samples; peak = std::max(peak, w.populationCount()); }
        }
        const double mean = popSum / static_cast<double>(samples);
        std::printf("ecology seed %u: mean %.1f, end %zu, peak %zu, births %llu, starved %llu\n", seed, mean, w.populationCount(), peak,
                    static_cast<unsigned long long>(w.stats().births), static_cast<unsigned long long>(w.stats().deathsStarved));
        WSIM_CHECK(w.populationCount() >= 5);                 // не вымерли
        WSIM_CHECK(mean >= 12.0);                              // живая популяция
        WSIM_CHECK(peak < World::kMaxPopulation);              // не упёрлись в потолок
        WSIM_CHECK(w.stats().births >= 10);                    // рождаются
        WSIM_CHECK(w.stats().deathsThirst < 5);                // жажда не косит
    }
    std::puts("ecology_tests: OK");
    return 0;
}

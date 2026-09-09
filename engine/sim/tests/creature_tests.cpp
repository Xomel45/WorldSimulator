#include "sim/Creature.hpp"

#include "test_util.hpp"

#include <cstdio>

using namespace wsim;

namespace {
constexpr Genome::TraitIndex kAllTraits[] = {
    Genome::kSpeed, Genome::kSize, Genome::kMetabolism, Genome::kLifespan, Genome::kFertility,
};
}

int main() {
    Rng rng(2024);
    Creature c;
    c.genome = Genome::random(rng);

    const float initialHunger = c.needs.hunger;
    const float initialEnergy = c.needs.energy;

    SenseInputs senses{};
    for (int i = 0; i < 100; ++i)
        c.tick(0.1f, senses);  // 10 секунд симулированного времени, dt=100мс шагами

    WSIM_CHECK(c.needs.hunger > initialHunger);
    WSIM_CHECK(c.needs.energy < initialEnergy);
    WSIM_CHECK(c.age > 9.9f && c.age < 10.1f);

    const float hungerBeforeFeed = c.needs.hunger;
    c.feed(0.3f);
    WSIM_CHECK(c.needs.hunger < hungerBeforeFeed);

    const float energyBeforeRest = c.needs.energy;
    c.rest(0.3f);
    WSIM_CHECK(c.needs.energy > energyBeforeRest);

    c.needs.libido = 0.7f;
    c.mate();
    WSIM_CHECK(c.needs.libido == 0.0f);

    // Смерть от старости: тикаем крупными шагами, пока не умрёт.
    Creature old;
    old.genome = Genome::random(rng);
    while (old.alive) old.tick(50.0f, senses);
    WSIM_CHECK(old.age >= old.lifespan());

    // Размножение: геном ребёнка валиден, стартовое состояние -- дефолтное.
    Rng rngR(55);
    Creature parentA;
    parentA.genome = Genome::random(rngR);
    parentA.position = {0.0f, 0.0f, 0.0f};
    Creature parentB;
    parentB.genome = Genome::random(rngR);
    parentB.position = {10.0f, 0.0f, 0.0f};

    Creature child = Creature::reproduce(parentA, parentB, rngR);
    WSIM_CHECK(child.alive);
    WSIM_CHECK(child.age == 0.0f);
    WSIM_CHECK(child.position.x > 4.9f && child.position.x < 5.1f);
    for (auto i : kAllTraits)
        WSIM_CHECK(child.genome.trait(i) >= -1.0f && child.genome.trait(i) <= 1.0f);

    std::puts("creature_tests: OK");
    return 0;
}

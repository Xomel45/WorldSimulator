#include "sim/Creature.hpp"

#include "test_util.hpp"

#include <cstdio>
#include <vector>

using namespace wsim;

namespace {
constexpr Genome::TraitIndex kAllTraits[] = {
    Genome::kSpeed, Genome::kSize, Genome::kMetabolism, Genome::kLifespan, Genome::kFertility,
    Genome::kPlasticity,
};
}

int main() {
    Rng rng(2024);
    Creature c;
    c.genome = Genome::random(rng);
    WSIM_CHECK(c.id == 0);                                  // не назначено (это забота мира)
    WSIM_CHECK(c.parentIds[0] == 0 && c.parentIds[1] == 0);  // не рождён через reproduce()
    WSIM_CHECK(c.bondedPartnerId == 0);

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
    c.mate(/*partnerId=*/42);
    WSIM_CHECK(c.needs.libido == 0.0f);
    WSIM_CHECK(c.bondedPartnerId == 42);

    // recordChild накапливает id детей, не перезаписывает.
    WSIM_CHECK(c.childIds.empty());
    c.recordChild(100);
    c.recordChild(101);
    WSIM_CHECK(c.childIds.size() == 2);
    WSIM_CHECK(c.childIds[0] == 100 && c.childIds[1] == 101);

    // Обучение в течение жизни: learnedWeights лениво инициализируется первым
    // tick() и дрейфует от genome.initialPlasticWeights() (голод растёт,
    // энергия падает при нейтральных senses -- устойчиво отрицательный reward
    // достаточен, чтобы веса сдвинулись), а сам geноme при этом не трогается
    // (Дарвин, не Ламарк).
    {
        Rng rngL(303);
        Creature learner;
        learner.genome = Genome::random(rngL);
        WSIM_CHECK(learner.learnedWeights.empty());

        const std::vector<float> genomeSnapshot(
            learner.genome.initialPlasticWeights().begin(), learner.genome.initialPlasticWeights().end());

        learner.tick(0.1f, senses);  // лениво инициализирует learnedWeights
        WSIM_CHECK(learner.learnedWeights.size() == genomeSnapshot.size());
        const std::vector<float> weightsAfterFirstTick = learner.learnedWeights;

        for (int i = 0; i < 200; ++i)
            learner.tick(0.1f, senses);

        WSIM_CHECK(learner.learnedWeights != weightsAfterFirstTick);  // обучение реально сдвинуло веса
        const std::vector<float> genomeAfter(
            learner.genome.initialPlasticWeights().begin(), learner.genome.initialPlasticWeights().end());
        WSIM_CHECK(genomeAfter == genomeSnapshot);  // геном не мутировал

        // resetLearning() возвращает к стартовой точке генома.
        learner.resetLearning();
        WSIM_CHECK(learner.learnedWeights == genomeSnapshot);
    }

    // Смерть от старости: тикаем крупными шагами, пока не умрёт.
    Creature old;
    old.genome = Genome::random(rng);
    while (old.alive) old.tick(50.0f, senses);
    WSIM_CHECK(old.age >= old.lifespan());

    // Размножение: геном ребёнка валиден, стартовое состояние -- дефолтное,
    // parentIds указывают на уже назначенные id родителей.
    Rng rngR(55);
    Creature parentA;
    parentA.genome = Genome::random(rngR);
    parentA.position = {0.0f, 0.0f, 0.0f};
    parentA.id = 7;
    Creature parentB;
    parentB.genome = Genome::random(rngR);
    parentB.position = {10.0f, 0.0f, 0.0f};
    parentB.id = 9;

    Creature child = Creature::reproduce(parentA, parentB, rngR);
    WSIM_CHECK(child.alive);
    WSIM_CHECK(child.age == 0.0f);
    WSIM_CHECK(child.id == 0);  // ещё не зарегистрирован миром
    WSIM_CHECK(child.parentIds[0] == 7 && child.parentIds[1] == 9);
    WSIM_CHECK(child.position.x > 4.9f && child.position.x < 5.1f);
    for (auto i : kAllTraits)
        WSIM_CHECK(child.genome.trait(i) >= -1.0f && child.genome.trait(i) <= 1.0f);

    std::puts("creature_tests: OK");
    return 0;
}

#include "sim/Creature.hpp"

#include "test_util.hpp"

#include <cstdio>
#include <vector>

using namespace wsim;

namespace {
constexpr Genome::TraitIndex kAllTraits[] = {
    Genome::kSpeed, Genome::kSize, Genome::kMetabolism, Genome::kLifespan, Genome::kFertility,
    Genome::kPlasticity, Genome::kBoldness, Genome::kSociability, Genome::kBondStrength, Genome::kEnvy,
    Genome::kTerritorial, Genome::kCuriosity, Genome::kEmpathy,
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
    const float initialRespect = c.needs.respect;

    SenseInputs senses{};
    for (int i = 0; i < 100; ++i)
        c.tick(0.1f, senses);  // 10 секунд симулированного времени, dt=100мс шагами

    WSIM_CHECK(c.needs.hunger > initialHunger);
    WSIM_CHECK(c.needs.energy < initialEnergy);
    WSIM_CHECK(c.needs.respect > initialRespect);  // фоновый дрейф, как у libido/social
    WSIM_CHECK(c.age > 9.9f && c.age < 10.1f);

    const float hungerBeforeFeed = c.needs.hunger;
    c.feed(0.3f);
    WSIM_CHECK(c.needs.hunger < hungerBeforeFeed);

    const float energyBeforeRest = c.needs.energy;
    c.rest(0.3f);
    WSIM_CHECK(c.needs.energy > energyBeforeRest);

    // respect дрейфует сам по себе (как libido/social) и падает через
    // earnRespect() -- тот же "мир решает, Creature только хранит" паттерн,
    // что и у feed()/rest().
    const float respectBeforeEarn = c.needs.respect;
    c.earnRespect(0.3f);
    WSIM_CHECK(c.needs.respect < respectBeforeEarn);

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

    // Явный темперамент реально что-то меняет: два одинаковых существа, кроме
    // гена boldness на разных полюсах ([-1] трус, [1] храбрец) -- под угрозой
    // трус боится сильнее.
    {
        Rng rngT(404);
        Creature timid;
        timid.genome = Genome::random(rngT);
        Creature brave = timid;  // тот же геном, кроме boldness ниже
        timid.genome.setTrait(Genome::kBoldness, -1.0f);
        brave.genome.setTrait(Genome::kBoldness, 1.0f);

        SenseInputs threatSenses{};
        threatSenses.threatProximity = 0.8f;

        timid.tick(0.1f, threatSenses);
        brave.tick(0.1f, threatSenses);
        WSIM_CHECK(brave.needs.fear < timid.needs.fear);
    }

    // Чужая демонстрация статуса рядом (nearbyDominanceLevel) реально пугает
    // -- та же механика, что и nearbyAlarmLevel, только для статуса.
    {
        Rng rngD(505);
        Creature calm;
        calm.genome = Genome::random(rngD);
        Creature intimidated = calm;

        SenseInputs plainSenses{};
        SenseInputs dominanceSenses{};
        dominanceSenses.nearbyDominanceLevel = 0.9f;

        calm.tick(0.1f, plainSenses);
        intimidated.tick(0.1f, dominanceSenses);
        WSIM_CHECK(intimidated.needs.fear > calm.needs.fear);
    }

    // Зависть/ненависть: у завистливой особи (envy=1) рядом с чужой
    // демонстрацией статуса обида (resentment) растёт, у независтливой
    // (envy=-1) -- нет (нет способности обижаться в принципе).
    {
        Rng rngE(606);
        Creature envious;
        envious.genome = Genome::random(rngE);
        Creature indifferent = envious;
        envious.genome.setTrait(Genome::kEnvy, 1.0f);
        indifferent.genome.setTrait(Genome::kEnvy, -1.0f);

        SenseInputs dominanceSenses{};
        dominanceSenses.nearbyDominanceLevel = 0.9f;

        envious.tick(0.1f, dominanceSenses);
        indifferent.tick(0.1f, dominanceSenses);
        WSIM_CHECK(envious.needs.resentment > indifferent.needs.resentment);
        WSIM_CHECK(indifferent.needs.resentment == 0.0f);
    }

    // Горе: смерть близкого (lovedOneDied) поднимает grief пропорционально
    // bondStrength() и медленно угасает без нового сигнала.
    {
        Rng rngG(1010);
        Creature mourner;
        mourner.genome = Genome::random(rngG);
        WSIM_CHECK(mourner.needs.grief == 0.0f);

        SenseInputs deathSenses{};
        deathSenses.lovedOneDied = 1.0f;
        mourner.tick(0.1f, deathSenses);
        WSIM_CHECK(mourner.needs.grief == mourner.bondStrength());
        const float griefAfterLoss = mourner.needs.grief;

        SenseInputs quietSenses{};
        for (int i = 0; i < 50; ++i)
            mourner.tick(0.1f, quietSenses);
        WSIM_CHECK(mourner.needs.grief < griefAfterLoss);  // угасает со временем
    }

    // Территориальность: после лениво зафиксированного дома, вдали от него
    // территориальная особь (territorial=1) боится сильнее равнодушной
    // (territorial=-1) на том же расстоянии.
    {
        Rng rngH(707);
        Creature homebody;
        homebody.genome = Genome::random(rngH);
        Creature wanderer = homebody;
        homebody.genome.setTrait(Genome::kTerritorial, 1.0f);
        wanderer.genome.setTrait(Genome::kTerritorial, -1.0f);

        SenseInputs emptySenses{};
        homebody.tick(0.1f, emptySenses);  // лениво фиксирует homePosition = текущая position
        wanderer.tick(0.1f, emptySenses);

        homebody.position = glm::vec3(100.0f, 0.0f, 0.0f);  // далеко от дома
        wanderer.position = glm::vec3(100.0f, 0.0f, 0.0f);

        homebody.tick(0.1f, emptySenses);
        wanderer.tick(0.1f, emptySenses);
        WSIM_CHECK(homebody.needs.fear > wanderer.needs.fear);
    }

    // Любопытство: при спокойных needs (низкая срочность) у любопытной особи
    // (curiosity=1) boredomLevel() выше, чем у равнодушной (curiosity=-1) --
    // чистая функция состояния, tick() не нужен.
    {
        Rng rngC(808);
        Creature curious;
        curious.genome = Genome::random(rngC);
        Creature indifferent = curious;
        curious.genome.setTrait(Genome::kCuriosity, 1.0f);
        indifferent.genome.setTrait(Genome::kCuriosity, -1.0f);

        WSIM_CHECK(curious.boredomLevel() > indifferent.boredomLevel());
        WSIM_CHECK(indifferent.boredomLevel() == 0.0f);
    }

    // Эмпатия к чужим: паника чужака (nearbyStrangerAlarmLevel, без паники
    // своих) пугает эмпатичную особь (empathy=1) сильнее равнодушной
    // (empathy=-1).
    {
        Rng rngEm(909);
        Creature compassionate;
        compassionate.genome = Genome::random(rngEm);
        Creature indifferentToStrangers = compassionate;
        compassionate.genome.setTrait(Genome::kEmpathy, 1.0f);
        indifferentToStrangers.genome.setTrait(Genome::kEmpathy, -1.0f);

        SenseInputs strangerAlarm{};
        strangerAlarm.nearbyStrangerAlarmLevel = 0.9f;

        compassionate.tick(0.1f, strangerAlarm);
        indifferentToStrangers.tick(0.1f, strangerAlarm);
        WSIM_CHECK(compassionate.needs.fear > indifferentToStrangers.needs.fear);
    }

    // Смерть от старости: сытый и отдохнувший доживает до конца срока (кормим и отдыхаем каждый шаг).
    Creature old;
    old.genome = Genome::random(rng);
    while (old.alive) {
        old.feed(1.0f);
        old.rest(1.0f);
        old.tick(5.0f, senses);
    }
    WSIM_CHECK(old.age >= old.lifespan());
    WSIM_CHECK(old.deathCause == 1);

    // Голод убивает: без еды существо умирает раньше срока (здоровье падает после hunger==1), причина -- голод.
    {
        Creature starving;
        starving.genome = Genome::random(rng);
        starving.genome.setTrait(Genome::kLifespan, 1.0f);  // долгая жизнь: дожить до старости было бы нечем, кроме еды
        float t = 0.0f;
        while (starving.alive && t < 5000.0f) { starving.tick(1.0f, senses); t += 1.0f; }
        WSIM_CHECK(!starving.alive);
        WSIM_CHECK(starving.deathCause == 2);
        WSIM_CHECK(starving.age < starving.lifespan());
    }

    // Зрелость и готовность: ребёнок не размножается, взрослый сытый и отдохнувший с либидо -- готов,
    // голодный/вымотанный/на перезарядке -- нет.
    {
        Creature a;
        a.genome = Genome::random(rng);
        WSIM_CHECK(!a.isMature() && !a.canMate());
        a.age = a.lifespan() * 0.5f;
        a.needs = Needs{};
        a.needs.libido = 0.8f; a.needs.hunger = 0.2f; a.needs.energy = 0.9f;
        WSIM_CHECK(a.isMature() && a.canMate());
        a.needs.hunger = 0.9f;   WSIM_CHECK(!a.canMate());
        a.needs.hunger = 0.2f;   a.needs.energy = 0.2f;   WSIM_CHECK(!a.canMate());
        a.needs.energy = 0.9f;   a.matingCooldown = 30.0f; WSIM_CHECK(!a.canMate());
        a.matingCooldown = 0.0f; a.health = 0.3f;         WSIM_CHECK(!a.canMate());
    }

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

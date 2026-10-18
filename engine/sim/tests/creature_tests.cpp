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

    // ── Вкус ягод, яд и недомогания ──
    {
        Rng r2(77);
        Creature t;
        t.genome = Genome::random(r2);
        t.age    = t.lifespan() * 0.3f;
        t.id     = 5;
        constexpr float kDt = 1.0f / 60.0f;
        SenseInputs idle{};
        auto eat = [&](Creature& c, BerryKind k, int ticks) {
            const BerryDef& d = berryDef(k);
            for (int i = 0; i < ticks; ++i) {
                c.ingestBerry(d, 0.35f * kDt);
                c.tasteBerry(k, d.nutrition, d.harm(), kDt);
            }
        };

        // Незнакомое манит с авансом по любопытству; знакомым становится после первой трапезы.
        for (int k = 0; k < kBerryKindCount; ++k) {
            const float a = t.berryAppeal(static_cast<BerryKind>(k));
            WSIM_CHECK(!t.knowsBerry(static_cast<BerryKind>(k)) && a >= 0.6f - 1e-4f && a <= 1.1f + 1e-4f);
        }
        Creature good = t, bad = t, mild = t;
        eat(good, BerryKind::Cloudberry, 120);
        eat(bad, BerryKind::Nightshade, 120);
        eat(mild, BerryKind::Baneberry, 120);
        WSIM_CHECK(good.knowsBerry(BerryKind::Cloudberry) && good.berryAppeal(BerryKind::Cloudberry) > 1.2f);  // морошка: сытная, нравится
        WSIM_CHECK(bad.berryAppeal(BerryKind::Nightshade) < -0.8f);                                             // волчья: смертельный вкус
        WSIM_CHECK(mild.berryAppeal(BerryKind::Baneberry) < Creature::kMinAppeal);                              // вороний глаз: тоже обходит
        WSIM_CHECK(!good.knowsBerry(BerryKind::Raspberry) && good.berryAppeal(BerryKind::Raspberry) >= 0.6f - 1e-4f);  // чужая группа не тронута

        // Мнение складывается о ВНЕШНЕМ ВИДЕ: съев хорошую ягоду, существо тянется к её подделке; отравившись подделкой --
        // отворачивается от настоящей. Ежевика и волчья ягода -- "тёмные", малина и жимолость -- "красные".
        Creature fan = t;
        eat(fan, BerryKind::Blackberry, 120);
        WSIM_CHECK(fan.knowsBerry(BerryKind::Nightshade) && fan.berryAppeal(BerryKind::Nightshade) > 1.0f);  // принимает подделку за ежевику
        eat(fan, BerryKind::Nightshade, 12);                                                                // и обжигается
        WSIM_CHECK(fan.berryAppeal(BerryKind::Blackberry) < fan.berryAppeal(BerryKind::Cloudberry));         // теперь и ежевика под подозрением
        Creature red = t;
        eat(red, BerryKind::Honeysuckle, 60);
        WSIM_CHECK(red.berryAppeal(BerryKind::Raspberry) == red.berryAppeal(BerryKind::Honeysuckle));       // одна группа

        // Мнение складывается быстро: ядовитая ягода становится непривлекательной за доли секунды.
        Creature quick = t;
        int ticks = 0;
        while (quick.berryAppeal(BerryKind::Nightshade) >= Creature::kMinAppeal && ticks < 600) {
            quick.tasteBerry(BerryKind::Nightshade, 0.6f, berryDef(BerryKind::Nightshade).harm(), kDt);
            ++ticks;
        }
        WSIM_CHECK(ticks >= 2 && ticks < 40);
        // Вкус тускнеет: за много минут отрицательное мнение возвращается к нейтральному (можно попробовать снова).
        Creature old = bad;
        for (int i = 0; i < 60 * 60 * 20; ++i) old.tick(kDt, idle);  // 20 минут
        WSIM_CHECK(!old.alive || old.berryAppeal(BerryKind::Nightshade) > -0.3f);

        // Смертельный яд: бьёт по здоровью и силам, сам выводится, убивает достаточной дозой (deathCause 4).
        Creature sick = t;
        sick.needs.energy = 0.9f;
        sick.addToxin(0.8f);
        const float h0 = sick.health;
        for (int i = 0; i < 300; ++i) sick.tick(kDt, idle);
        WSIM_CHECK(sick.alive && sick.health < h0 && sick.toxin < 0.8f && sick.toxin > 0.0f && sick.sick());
        Creature dying = t;
        dying.health = 0.15f;
        dying.addToxin(2.0f);
        for (int i = 0; i < 60 * 30 && dying.alive; ++i) dying.tick(kDt, idle);
        WSIM_CHECK(!dying.alive && dying.deathCause == 4);
        Creature capped = t;
        capped.addToxin(50.0f);
        WSIM_CHECK(capped.toxin <= 2.0f);

        // У каждой ягоды свой профиль: безопасные не вредят; крушина -- только понос; жимолость -- только рвота; белена --
        // галлюцинации; вороний глаз -- рвота и понос; волчья -- смерть.
        const BerryDef& rasp = berryDef(BerryKind::Raspberry);
        WSIM_CHECK(!isPoisonous(BerryKind::Raspberry) && rasp.lethal == 0 && rasp.diarrhea == 0 && rasp.vomit == 0 && rasp.hallucination == 0);
        for (auto k : {BerryKind::Blueberry, BerryKind::Blackberry, BerryKind::Cloudberry, BerryKind::Tropical, BerryKind::Cactus}) WSIM_CHECK(!isPoisonous(k));
        auto only = [](BerryKind k, bool lethal, bool diarrhea, bool vomit, bool hallu) {
            const BerryDef& d = berryDef(k);
            return (d.lethal > 0) == lethal && (d.diarrhea > 0) == diarrhea && (d.vomit > 0) == vomit && (d.hallucination > 0) == hallu;
        };
        WSIM_CHECK(only(BerryKind::Buckthorn, false, true, false, false));
        WSIM_CHECK(only(BerryKind::Honeysuckle, false, false, true, false));
        WSIM_CHECK(only(BerryKind::Henbane, false, false, false, true));
        WSIM_CHECK(only(BerryKind::Baneberry, false, true, true, false));
        WSIM_CHECK(berryDef(BerryKind::Nightshade).lethal > 0.5f);

        // Понос: еда проскакивает (голод растёт быстрее обычного), силы тают, идёт медленнее; проходит сам, не убивает.
        Creature runny = t, ctrl = t;
        runny.needs.hunger = ctrl.needs.hunger = 0.2f;
        runny.needs.energy = ctrl.needs.energy = 0.9f;
        eat(runny, BerryKind::Buckthorn, 120);
        WSIM_CHECK(runny.diarrhea > 0.5f && runny.toxin == 0.0f && runny.sick());
        for (int i = 0; i < 600; ++i) { runny.tick(kDt, idle); ctrl.tick(kDt, idle); }
        WSIM_CHECK(runny.needs.hunger > ctrl.needs.hunger + 0.1f && runny.needs.energy < ctrl.needs.energy);
        WSIM_CHECK(runny.alive && runny.health > 0.9f);
        for (int i = 0; i < 60 * 120; ++i) runny.tick(kDt, idle);
        WSIM_CHECK(runny.diarrhea < 0.05f);  // прошёл

        // Рвота: тошнота копится, на пороге -- приступ (стоит, желудок пуст, яд частично выведен), потом отпускает.
        Creature queasy = t;
        queasy.needs.hunger = 0.1f;
        queasy.addToxin(0.6f);
        eat(queasy, BerryKind::Honeysuckle, 120);
        WSIM_CHECK(queasy.nausea > 0.5f && queasy.vomitStun == 0.0f);
        bool vomited = false, stoodStill = true;
        float toxinBefore = queasy.toxin;
        for (int i = 0; i < 60 * 30; ++i) {
            const float x0 = queasy.position.x, z0 = queasy.position.z;
            const bool  wasStunned = queasy.vomitStun > 0.0f;  // приступ уже идёт на входе в тик
            queasy.tick(kDt, idle);
            vomited |= queasy.vomitStun > 0.0f;
            if (wasStunned) stoodStill &= queasy.position.x == x0 && queasy.position.z == z0 && queasy.behavior == Behavior::Sick;
        }
        WSIM_CHECK(vomited && stoodStill);
        WSIM_CHECK(queasy.needs.hunger > 0.25f);       // желудок опустел
        WSIM_CHECK(queasy.toxin < toxinBefore * 0.7f);  // и яд вышел вместе с ним

        // Галлюцинации: мерещатся угрозы -- мечется и зовёт на помощь без причины, курс плывёт; через время проходит.
        Creature high = t, sober = t;
        for (Creature* c : {&high, &sober}) { c->needs.hunger = 0.1f; c->needs.energy = 1.0f; }
        eat(high, BerryKind::Henbane, 120);
        WSIM_CHECK(high.hallucination > 0.5f && high.toxin == 0.0f);
        int alarms = 0, soberAlarms = 0;
        float maxFearHigh = 0.0f, maxFearSober = 0.0f;
        for (int i = 0; i < 60 * 40; ++i) {
            high.needs.hunger = sober.needs.hunger = 0.1f;
            high.needs.energy = sober.needs.energy = 1.0f;
            const BrainOutputs oh = high.tick(kDt, idle), os = sober.tick(kDt, idle);
            alarms += oh.alarmCall > 0.5f;
            soberAlarms += os.alarmCall > 0.5f;
            maxFearHigh = std::max(maxFearHigh, high.needs.fear);
            maxFearSober = std::max(maxFearSober, sober.needs.fear);
        }
        WSIM_CHECK(alarms > 20 && soberAlarms == 0);   // кричит без причины, трезвый -- нет
        WSIM_CHECK(maxFearHigh > 0.6f && maxFearSober < 0.3f);
        for (int i = 0; i < 60 * 120; ++i) { high.needs.hunger = 0.1f; high.needs.energy = 1.0f; high.tick(kDt, idle); }  // сытый и бодрый: проверяем только сам эффект
        WSIM_CHECK(high.hallucination < 0.05f && high.health > 0.9f);  // прошло, и здоровью ничего не сделало (не яд)

        // Вкус учитывает все недомогания: чем хуже, тем больше harm; смерть -- хуже всего.
        WSIM_CHECK(berryDef(BerryKind::Nightshade).harm() > berryDef(BerryKind::Baneberry).harm());
        WSIM_CHECK(berryDef(BerryKind::Baneberry).harm() > berryDef(BerryKind::Buckthorn).harm() * 0.9f);
        WSIM_CHECK(berryDef(BerryKind::Raspberry).harm() == 0.0f);

        // Вкус и недомогания не наследуются: ребёнок чистый (дарвиновски, как learnedWeights).
        Rng rr(5);
        Creature pa = bad, pb = good;
        pa.id = 1; pb.id = 2;
        pa.diarrhea = pa.nausea = pa.hallucination = 1.0f;
        const Creature kid = Creature::reproduce(pa, pb, rr);
        WSIM_CHECK(kid.toxin == 0.0f && kid.diarrhea == 0.0f && kid.nausea == 0.0f && kid.hallucination == 0.0f && !kid.sick());
        for (int k = 0; k < kBerryLookCount; ++k) WSIM_CHECK(!kid.knowsLook(static_cast<BerryLook>(k)) && kid.taste[static_cast<std::size_t>(k)] == 0.0f);
    }

    std::puts("creature_tests: OK");
    return 0;
}

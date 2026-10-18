#include "sim/Creature.hpp"

#include "sim/Behavior.hpp"
#include "sim/Terrain.hpp"
#include "sim/Trees.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace wsim {

namespace {

float lerpTrait(float gene, float lo, float hi) noexcept {
    return lo + (gene * 0.5f + 0.5f) * (hi - lo);  // gene in [-1,1] -> [lo,hi]
}

// Детерминированный "шум" из возраста — без хранения RNG-состояния в Creature
// и без внешнего Rng& в горячем tick(). Не криптографического качества, но и
// не нужно: единственная цель — разрывать идеально симметричные решения мозга.
// Детерминированный "случайный" 0..1 по существу, полусекундному отрезку возраста и соли (для галлюцинаций).
float hash01(std::uint64_t id, float age, std::uint64_t salt) noexcept {
    std::uint64_t h = id * 0x9E3779B97F4A7C15ull + static_cast<std::uint64_t>(std::floor(age * 2.0f)) * 0xBF58476D1CE4E5B9ull + salt * 0x94D049BB133111EBull;
    h ^= h >> 31; h *= 0x94D049BB133111EBull; h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0x1000000);
}

float noiseFromAge(float age) noexcept {
    std::uint32_t bits;
    std::memcpy(&bits, &age, sizeof(bits));
    bits ^= bits << 13;
    bits ^= bits >> 17;
    bits ^= bits << 5;
    return static_cast<float>(bits) / static_cast<float>(0xFFFFFFFFu) * 2.0f - 1.0f;
}

// Простой скаляр "как дела" из основных драйвов выживания — разница до/после
// тика и есть reward для reinforce() (см. Creature::tick()).
float wellbeing(const Needs& n) noexcept { return -n.hunger - (1.0f - n.energy) - n.fear; }

// Насколько велика "домашняя зона" для awayFromHome() -- плейсхолдер-масштаб
// до системы террейна/мира с реальными единицами (тот же плейсхолдер-юнит
// подход, что и у lifespan()).
constexpr float kHomeRangeScale = 20.0f;

// Физика рельефа (slope = подъём/пробег, 1.0 = 45 градусов).
constexpr float kProbeDistance   = 1.5f;   // на сколько вперёд смотрим, чтобы оценить уклон
constexpr float kClimbSlowdown   = 2.5f;   // скорость на подъёме: 1 / (1 + k*slope)
constexpr float kClimbEffort     = 4.0f;   // расход энергии на подъёме: 1 + k*slope
constexpr float kMaxClimbSlope   = 1.2f;   // круче -- обрыв, не взобраться (~50 градусов)
constexpr float kDownhillBoost   = 0.2f;   // под гору чуть быстрее (до +20%)
constexpr float kWaterSpeed      = 0.35f;  // уже оказавшись в воде, существо барахтается
constexpr float kWaterEffort     = 3.0f;

}  // namespace

float Creature::maxSpeed() const noexcept   { return lerpTrait(genome.trait(Genome::kSpeed), 1.0f, 6.0f); }
float Creature::size() const noexcept       { return lerpTrait(genome.trait(Genome::kSize), 0.7f, 1.4f); }
float Creature::metabolism() const noexcept { return lerpTrait(genome.trait(Genome::kMetabolism), 0.7f, 1.4f); }
float Creature::lifespan() const noexcept   { return lerpTrait(genome.trait(Genome::kLifespan), 300.0f, 1200.0f); }
float Creature::fertility() const noexcept  { return lerpTrait(genome.trait(Genome::kFertility), 0.0f, 1.0f); }
float Creature::plasticity() const noexcept { return lerpTrait(genome.trait(Genome::kPlasticity), 0.0f, 0.05f); }

float Creature::boldness() const noexcept     { return lerpTrait(genome.trait(Genome::kBoldness), 0.0f, 1.0f); }
float Creature::sociability() const noexcept  { return lerpTrait(genome.trait(Genome::kSociability), 0.0f, 1.0f); }
float Creature::bondStrength() const noexcept { return lerpTrait(genome.trait(Genome::kBondStrength), 0.2f, 1.0f); }
float Creature::envy() const noexcept          { return lerpTrait(genome.trait(Genome::kEnvy), 0.0f, 1.0f); }
float Creature::territorial() const noexcept   { return lerpTrait(genome.trait(Genome::kTerritorial), 0.0f, 1.0f); }
float Creature::curiosity() const noexcept     { return lerpTrait(genome.trait(Genome::kCuriosity), 0.0f, 1.0f); }
float Creature::empathy() const noexcept       { return lerpTrait(genome.trait(Genome::kEmpathy), 0.0f, 1.0f); }

float Creature::awayFromHome() const noexcept {
    const float homeDistance = glm::length(position - homePosition);
    return homeDistance / (homeDistance + kHomeRangeScale);
}

float Creature::boredomLevel() const noexcept {
    const float urgency = std::max({needs.hunger, 1.0f - needs.energy, needs.fear, needs.libido});
    return (1.0f - urgency) * curiosity();
}

bool Creature::isMature() const noexcept { return age >= 0.1f * lifespan(); }

bool Creature::canMate() const noexcept {
    return isMature() && matingCooldown <= 0.0f && needs.libido >= 0.4f && needs.energy >= 0.45f && needs.hunger <= 0.4f && health >= 0.6f;
}

const char* behaviorName(Behavior b) noexcept {
    switch (b) {
        case Behavior::Wander:    return "Бродит";
        case Behavior::SeekFood:  return "Ищет еду";
        case Behavior::Eat:       return "Ест";
        case Behavior::Rest:      return "Отдыхает";
        case Behavior::Flee:      return "Убегает";
        case Behavior::SeekMate:  return "Ищет пару";
        case Behavior::Socialize: return "Тянется к своим";
        case Behavior::GoHome:    return "Идёт домой";
        case Behavior::Sick:      return "Тошнит";
        case Behavior::Sabotage:  return "Мстит";
        case Behavior::Count:     break;
    }
    return "?";
}

void Creature::resetLearning() noexcept {
    const auto initial = genome.initialPlasticWeights();
    learnedWeights.assign(initial.begin(), initial.end());
}

BrainOutputs Creature::tick(float dt, const SenseInputs& senses) noexcept {
    if (learnedWeights.empty()) {
        resetLearning();
        homePosition = position;  // "дом" -- где существо впервые очнулось, а не назначение мира
    }
    const float wellbeingBefore = wellbeing(needs);

    const BrainInputs in{
        .hunger  = needs.hunger,
        .energy  = needs.energy,
        .fear    = needs.fear,
        .libido  = needs.libido,
        .social  = needs.social,
        .respect = needs.respect,
        .resentment = needs.resentment,
        .grief   = needs.grief,

        .foodDirX     = senses.foodDir.x,
        .foodDirZ     = senses.foodDir.y,
        .foodProximity = senses.foodProximity,

        .threatDirX     = senses.threatDir.x,
        .threatDirZ     = senses.threatDir.y,
        .threatProximity = senses.threatProximity,

        .mateDirX     = senses.mateDir.x,
        .mateDirZ     = senses.mateDir.y,
        .mateProximity = senses.mateProximity,

        .familiarCrowding = senses.familiarCrowding,
        .strangerCrowding = senses.strangerCrowding,
        .noise            = noiseFromAge(age),

        .nearbyFamiliarAlarmLevel = senses.nearbyFamiliarAlarmLevel,
        .nearbyStrangerAlarmLevel = senses.nearbyStrangerAlarmLevel,
        .nearbyMatingCallLevel    = senses.nearbyMatingCallLevel,
        .nearbyDominanceLevel     = senses.nearbyDominanceLevel,

        .selfSize = size(),

        .lovedOneDirX      = senses.lovedOneDir.x,
        .lovedOneDirZ      = senses.lovedOneDir.y,
        .lovedOneProximity = senses.lovedOneProximity,
        .lovedOneThreatLevel = senses.lovedOneThreatLevel,

        .awayFromHome = awayFromHome(),
        .boredom      = boredomLevel(),
    };

    const brain::BrainEvalResult result = brain::evaluate(genome.fixedBrainWeights(), learnedWeights, in);
    const BrainOutputs& out = result.outputs;

    const float metab = metabolism();
    needs.hunger = std::clamp(needs.hunger + 0.01f * metab * dt, 0.0f, 1.0f);

    // Слой поведения решает, что делать (мозг -- модулятор приоритетов и источник личного уклона, см. Behavior.hpp).
    behavior::Decision dec = behavior::decide(*this, senses, out, dt);

    // Недомогание перебивает решения. Рвота: приступ -- стоит, не ест (мир видит это как "Тошнит"). Галлюцинации:
    // порой мерещится угроза -- мчится прочь от несуществующего и зовёт на помощь (соседи пугаются по-настоящему),
    // а в остальное время курс слегка "плывёт".
    if (vomitStun > 0.0f) {
        vomitStun = std::max(0.0f, vomitStun - dt);
        dec.move = glm::vec2(0.0f);
        dec.eat = dec.rest = dec.mate = dec.pick = dec.serve = false;
        dec.matingCall = 0.0f;
        behavior = Behavior::Sick;
    } else if (hallucination > 0.05f) {
        visionTimer = std::max(0.0f, visionTimer - dt);
        if (visionTimer <= 0.0f) {
            const float roll = hash01(id, age, 11);
            if (roll < std::min(0.5f, hallucination * 0.6f)) {
                visionTimer   = 1.0f + 1.5f * hash01(id, age, 12);
                visionHeading = 6.2831853f * hash01(id, age, 13);
                needs.fear    = std::max(needs.fear, 0.7f);
            }
        }
        if (visionTimer > 0.0f) {
            dec.move  = glm::vec2(std::cos(visionHeading), std::sin(visionHeading));
            dec.eat   = dec.pick = dec.serve = false;
            dec.alarm = std::max(dec.alarm, 0.8f);
            behavior  = Behavior::Flee;
        } else if (glm::length(dec.move) > 0.01f) {
            const float a = (hash01(id, age, 14) - 0.5f) * 1.6f * std::min(hallucination, 1.0f);  // курс плывёт
            const float c = std::cos(a), s = std::sin(a);
            dec.move = {dec.move.x * c - dec.move.y * s, dec.move.x * s + dec.move.y * c};
        }
    }
    pickRequest  = dec.pick ? 1 : 0;   // мир выполнит (подобрать ягоды у куста / подсунуть обидчику)
    serveRequest = dec.serve ? 1 : 0;
    glm::vec2 moveDir = dec.move;

    // Рельеф: чем круче подъём в выбранную сторону, тем медленнее и тяжелее идти;
    // в воду с берега зайти нельзя. Мозг про рельеф не знает -- он выражает желание,
    // а физика его ограничивает (как и у всех остальных "решений" мира).
    const Terrain* terrain = senses.terrain;
    float speedMul = 1.0f, effortMul = 1.0f;
    const bool inWater = terrain && terrain->isUnderwater(position.x, position.z);
    if (terrain && glm::length(moveDir) > 0.01f) {
        const glm::vec2 dir = glm::normalize(moveDir);
        const float h0 = terrain->heightAt(position.x, position.z);
        const float h1 = terrain->heightAt(position.x + dir.x * kProbeDistance, position.z + dir.y * kProbeDistance);
        const float slope = (h1 - h0) / kProbeDistance;  // > 0 -- в гору
        if (slope > 0.0f) {
            speedMul  = slope > kMaxClimbSlope ? 0.0f : 1.0f / (1.0f + kClimbSlowdown * slope);
            effortMul = 1.0f + kClimbEffort * slope;
        } else {
            speedMul = 1.0f + kDownhillBoost * std::min(-slope, 1.0f);
        }
    }
    if (inWater) { speedMul *= kWaterSpeed; effortMul *= kWaterEffort; }
    if (needs.energy < 0.1f) speedMul *= 0.5f;  // вымотан -- еле идёт
    speedMul *= 1.0f - 0.3f * std::min(diarrhea, 1.0f);  // понос: идёт медленнее
    const float moveCost = glm::length(moveDir) * 0.01f * effortMul;
    needs.energy = std::clamp(needs.energy - (0.008f * metab + moveCost) * dt, 0.0f, 1.0f);

    // Страх реагирует на собственную угрозу, панику соседей (крик тревоги
    // распространяется по стае, даже если сама угроза не видна -- отдельно
    // для своих и чужих, см. empathy ниже), опасность близкого (вместе с
    // defendDesire/движением это и даёт "защиту любимых"), демонстрацию
    // статуса соседей (чужая демонстрация доминирования реально пугает --
    // та же механика, что и herd-паника от alarmCall, но завистливые особи
    // (envy) конвертируют часть этого испуга в обиду вместо покорности --
    // см. needs.resentment ниже), лёгкую фоновую настороженность от толпы
    // незнакомцев и дискомфорт вдали от дома (territorial) -- каждый вклад
    // промодулирован явным темпераментом (boldness/bondStrength/
    // sociability/envy/territorial/empathy), а не одинаковой для всех
    // константой.
    const float fearLerp = std::clamp(8.0f * dt, 0.0f, 1.0f);
    const float boldnessFactor = 1.0f - boldness() * 0.7f;  // пол 0.3: полностью бесстрашных не бывает
    const float fearTarget = std::max({
        senses.threatProximity * boldnessFactor,
        senses.nearbyFamiliarAlarmLevel * 0.6f * boldnessFactor,
        // Чужая паника по умолчанию почти не трогает (пол 0.2) -- эмпатичные
        // особи (empathy) чувствуют почти столько же сочувствия, сколько к
        // своим (до 0.8 при empathy=1).
        senses.nearbyStrangerAlarmLevel * 0.6f * boldnessFactor * (0.2f + empathy() * 0.6f),
        senses.lovedOneThreatLevel * 0.7f * bondStrength(),
        senses.strangerCrowding * 0.15f * (1.0f - sociability() * 0.6f),
        senses.nearbyDominanceLevel * 0.5f * boldnessFactor * (1.0f - envy() * 0.5f),
        awayFromHome() * 0.3f * territorial(),
    });
    needs.fear = needs.fear + (fearTarget - needs.fear) * fearLerp;

    // Свои удовлетворяют потребность в общении сильнее чужих (но не до нуля —
    // какая-то социальная ценность есть и в компании незнакомцев). Экстраверты
    // (высокий sociability) получают больше отдачи от любой компании;
    // базовый фоновый спад (-0.01) не зависит от темперамента -- одиночество
    // со временем чувствуют все, просто с разной скоростью восполнения.
    needs.social = std::clamp(
        needs.social +
            ((senses.familiarCrowding * 0.06f + senses.strangerCrowding * 0.02f) * (0.5f + sociability()) - 0.01f) *
                dt,
        0.0f, 1.0f);
    // Желание быть уважаемым растёт само по себе фоново (как libido/social),
    // и быстрее -- когда рядом кто-то демонстрирует статус (соперничество:
    // быть "переигранным" чужой демонстрацией подстёгивает желание доказать
    // своё). Удовлетворяется только миром через earnRespect(), когда
    // демонстрация этого существа реально выигрывает контест.
    needs.respect = std::clamp(
        needs.respect + (0.015f + 0.02f * senses.nearbyDominanceLevel) * dt, 0.0f, 1.0f);
    // Обида/зависть копится только у завистливых особей (envy) рядом с чужой
    // демонстрацией статуса и сама угасает без подпитки -- при envy=0 остаётся
    // ровно 0 всегда (нет способности обижаться в принципе). Мозг сам решает
    // (через эволюционировавшие веса), во что это выльется -- агрессию
    // (defendDesire), сближение или ничего -- см. BrainInputs::resentment.
    needs.resentment = std::clamp(
        needs.resentment + (senses.nearbyDominanceLevel * envy() * 0.05f - 0.02f) * dt, 0.0f, 1.0f);
    // Горе: смерть близкого (одноразовый сигнал мира) бьёт пропорционально
    // тому, насколько был дорог (bondStrength) -- сильно привязанные горюют
    // сильнее. Без нового сигнала само угасает, но чем крепче была связь,
    // тем медленнее ("время лечит" быстрее у равнодушных).
    if (senses.lovedOneDied > 0.0f) {
        needs.grief = bondStrength();
    } else {
        const float griefFade = 0.005f + 0.01f * (1.0f - bondStrength());
        needs.grief = std::clamp(needs.grief - griefFade * dt, 0.0f, 1.0f);
    }
    // Брачный зов соседей слегка подстёгивает либидо — социальная реплика
    // готовности к размножению, помимо собственного внутреннего дрейфа.
    needs.libido = std::clamp(
        needs.libido + (isMature() ? (0.02f * fertility() + 0.01f * senses.nearbyMatingCallLevel) * dt : 0.0f), 0.0f, 1.0f);

    glm::vec3 step = glm::vec3(moveDir.x, 0.0f, moveDir.y) * maxSpeed() * speedMul * dt;
    if (terrain) {
        // В воду с суши не заходим; упёрлись в берег под углом -- скользим вдоль него.
        // Стволы деревьев непроходимы (кроме случая, когда существо уже внутри ствола -- не запираем его).
        const bool inTrunk = Trees::blocksPoint(*terrain, position.x, position.z, 0.3f);
        auto blocked = [&](const glm::vec3& d) {
            if (!inWater && terrain->isUnderwater(position.x + d.x, position.z + d.z)) return true;
            return !inTrunk && Trees::blocksPoint(*terrain, position.x + d.x, position.z + d.z, 0.3f);
        };
        if (blocked(step)) {
            const glm::vec3 alongX{step.x, 0.0f, 0.0f}, alongZ{0.0f, 0.0f, step.z};
            if (!blocked(alongX))      step = alongX;
            else if (!blocked(alongZ)) step = alongZ;
            else                       step = glm::vec3(0.0f);
        }
    }
    position += step;
    velocity = dt > 0.0f ? step / dt : glm::vec3(0.0f);
    if (terrain) position.y = terrain->heightAt(position.x, position.z);
    age += dt;
    if (age >= lifespan()) { alive = false; deathCause = 1; }

    // Здоровье: голод и истощение бьют, сытость и силы лечат. Без этого еда ни на что не влияла бы:
    // никто не умирал от голода, и отбор было нечем вести.
    matingCooldown = std::max(0.0f, matingCooldown - dt);
    if (needs.hunger >= 0.95f) health -= 0.03f * dt;
    if (needs.energy <= 0.02f) health -= 0.012f * dt;
    if (needs.hunger < 0.6f && needs.energy > 0.2f) health = std::min(1.0f, health + 0.01f * dt);
    // Яд: пока он в крови, здоровье и силы тают, сам выводится. Вкус со временем тускнеет к нейтральному: через
    // четверть часа и ядовитая ягода "забывается" настолько, что её можно попробовать снова (иначе голодающий не мог бы
    // вернуться к кусту, о который когда-то обжёгся).
    if (toxin > 0.0f) {
        health -= toxin * 0.02f * dt;
        needs.energy = std::max(0.0f, needs.energy - toxin * 0.01f * dt);
        toxin        = std::max(0.0f, toxin - 0.03f * dt);
    }
    // Понос: питательное не усваивается -- голод растёт, силы уходят. Тошнота копится от рвотных ягод; на пороге --
    // приступ: стоит 1.6 с, желудок опустошён (голод +0.22), половина яда выведена, остаточная тошнота. Галлюцинации
    // и всё остальное медленно проходят.
    if (diarrhea > 0.0f) {
        needs.hunger = std::clamp(needs.hunger + diarrhea * 0.03f * dt, 0.0f, 1.0f);
        needs.energy = std::max(0.0f, needs.energy - diarrhea * 0.012f * dt);
        diarrhea     = std::max(0.0f, diarrhea - 0.025f * dt);
    }
    if (nausea > 0.0f) {
        if (nausea >= 1.0f && vomitStun <= 0.0f) {
            vomitStun    = 1.6f;
            sickEvent    = 1;
            needs.hunger = std::clamp(needs.hunger + 0.22f, 0.0f, 1.0f);
            toxin       *= 0.5f;
            nausea       = 0.25f;
        } else {
            nausea = std::max(0.0f, nausea - 0.04f * dt);
        }
    }
    if (hallucination > 0.0f) hallucination = std::max(0.0f, hallucination - 0.02f * dt);
    sinceBite = std::min(sinceBite + dt, 1000.0f);
    if (carryAmount > 0.0f && (carryAge += dt) > 45.0f) { carryAmount = 0.0f; carryKind = 255; }  // не донёс -- ягоды пропали
    if (grudge > 0.0f && grudgeId == 0) grudge = 0.0f;
    for (std::size_t k = 0; k < taste.size(); ++k)
        if (tasteSamples[k] > 0) taste[k] += (0.5f - taste[k]) * 0.002f * dt;
    if (health <= 0.0f && alive) { alive = false; deathCause = toxin > 0.05f ? 4 : (needs.hunger >= 0.95f ? 2 : 3); }

    // Reward-modulated Hebbian: подкрепляем/ослабляем связи, которые только
    // что сработали, в зависимости от того, стало ли лучше или хуже за этот
    // тик. Обучение затрагивает только learnedWeights (рантайм-копия) — сам
    // genome.initialPlasticWeights() этим тиком не трогается (не наследуется).
    const float reward = std::clamp(wellbeing(needs) - wellbeingBefore, -1.0f, 1.0f);
    brain::reinforce(learnedWeights, result, reward, plasticity());

    // Наружу -- то, что существо РЕШИЛО (мир резолвит это, соседи слышат сигналы); сырой выход мозга остаётся
    // внутри (им считается обучение выше).
    BrainOutputs act = out;
    act.moveX = dec.move.x;
    act.moveZ = dec.move.y;
    act.eatDesire    = dec.eat ? 1.0f : 0.0f;
    act.restDesire   = dec.rest ? 1.0f : 0.0f;
    act.mateDesire   = dec.mate ? 1.0f : 0.0f;
    act.fleeDesire   = behavior == Behavior::Flee ? 1.0f : 0.0f;
    act.alarmCall    = dec.alarm;
    act.matingCall   = dec.matingCall;
    return act;
}

void Creature::feed(float amount) noexcept { needs.hunger = std::clamp(needs.hunger - amount, 0.0f, 1.0f); }

float Creature::berryAppeal(BerryKind kind) const noexcept {
    const auto k = static_cast<std::size_t>(lookOf(kind));
    if (tasteSamples[k] == 0) return 0.6f + 0.5f * curiosity();  // незнакомое: любопытные пробуют охотнее
    return taste[k];
}

void Creature::tasteBerry(BerryKind kind, float nutrition, float harm, float dt) noexcept {
    constexpr float kLearnRate  = 3.0f;   // 1/с: пара десятых секунды -- и мнение уже сложилось
    const auto k = static_cast<std::size_t>(lookOf(kind));  // мнение -- о внешнем виде: подделки неотличимы
    if (tasteSamples[k] == 0) taste[k] = berryAppeal(kind);  // начинаем с аванса, а не с нуля
    const float experience = nutrition - harm;
    // Плохое запоминается втрое быстрее хорошего (как у настоящих животных): одной тошноты хватает на недели.
    constexpr float kBadnessBias = 3.0f;
    const float speed = kLearnRate * (0.6f + 0.8f * plasticity() / 0.05f) * (experience < taste[k] ? kBadnessBias : 1.0f);
    const float before = taste[k];
    taste[k] += (experience - taste[k]) * (1.0f - std::exp(-speed * dt));
    if (before >= kMinAppeal && taste[k] < kMinAppeal) sickEvent = 1;  // скривилось и отвернулось -- соседи это видят
    if (tasteSamples[k] < 255) ++tasteSamples[k];
    mealLook  = static_cast<std::uint8_t>(k);  // запомнили, что съели и когда -- если станет плохо, соседи тоже узнают
    sinceBite = 0.0f;
}

void Creature::addToxin(float amount) noexcept { toxin = std::clamp(toxin + amount, 0.0f, 2.0f); }

void Creature::ingestBerry(const BerryDef& def, float bite) noexcept {
    constexpr float kDose = 4.0f;  // доза за единицу съеденного при полной силе: ~0.3 с -- ощутимо, секунда -- тяжело
    const float toxin0 = toxin, diarrhea0 = diarrhea, hallu0 = hallucination;
    if (def.lethal > 0.0f)        addToxin(bite * def.lethal * kDose);
    if (def.diarrhea > 0.0f)      diarrhea      = std::clamp(diarrhea + bite * def.diarrhea * kDose, 0.0f, 2.0f);
    if (def.vomit > 0.0f)         nausea        = std::clamp(nausea + bite * def.vomit * kDose, 0.0f, 2.0f);
    if (def.hallucination > 0.0f) hallucination = std::clamp(hallucination + bite * def.hallucination * kDose, 0.0f, 2.0f);
    // Стало заметно плохо -- сигнал соседям (рвотный приступ добавляет tick()).
    if ((toxin > 0.3f && toxin0 <= 0.3f) || (diarrhea > 0.4f && diarrhea0 <= 0.4f) || (hallucination > 0.4f && hallu0 <= 0.4f)) sickEvent = 1;
}

void Creature::observeLook(BerryLook look, float experience, float weight) noexcept {
    const auto k = static_cast<std::size_t>(look);
    if (tasteSamples[k] == 0) taste[k] = 0.6f + 0.5f * curiosity();  // незнакомое: начинаем с аванса
    taste[k] += (experience - taste[k]) * std::clamp(weight, 0.0f, 1.0f);
    if (tasteSamples[k] < 255) ++tasteSamples[k];
}
void Creature::rest(float amount) noexcept { needs.energy = std::clamp(needs.energy + amount, 0.0f, 1.0f); }

void Creature::mate(std::uint64_t partnerId) noexcept {
    needs.libido = 0.0f;
    bondedPartnerId = partnerId;
}

void Creature::recordChild(std::uint64_t childId) { childIds.push_back(childId); }

void Creature::earnRespect(float amount) noexcept { needs.respect = std::clamp(needs.respect - amount, 0.0f, 1.0f); }

Creature Creature::reproduce(const Creature& a, const Creature& b, Rng& rng) {
    Creature child;
    child.genome = Genome::crossover(a.genome, b.genome, rng);
    child.genome.mutate(rng, /*rate=*/0.05f, /*strength=*/0.15f);
    child.position = (a.position + b.position) * 0.5f;
    child.parentIds = {a.id, b.id};
    child.needs.hunger = 0.25f;  // родился не голодным и не вымотанным
    child.needs.energy = 0.8f;
    return child;
}

}  // namespace wsim

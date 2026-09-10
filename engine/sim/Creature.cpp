#include "sim/Creature.hpp"

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

    glm::vec2 moveDir{out.moveX, out.moveZ};
    if (const float len = glm::length(moveDir); len > 1.0f) moveDir /= len;
    const float moveCost = glm::length(moveDir) * 0.01f;
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
        needs.libido + (0.02f * fertility() + 0.01f * senses.nearbyMatingCallLevel) * dt, 0.0f, 1.0f);

    velocity = glm::vec3(moveDir.x, 0.0f, moveDir.y) * maxSpeed();
    position += velocity * dt;
    age += dt;
    if (age >= lifespan()) alive = false;

    // Reward-modulated Hebbian: подкрепляем/ослабляем связи, которые только
    // что сработали, в зависимости от того, стало ли лучше или хуже за этот
    // тик. Обучение затрагивает только learnedWeights (рантайм-копия) — сам
    // genome.initialPlasticWeights() этим тиком не трогается (не наследуется).
    const float reward = std::clamp(wellbeing(needs) - wellbeingBefore, -1.0f, 1.0f);
    brain::reinforce(learnedWeights, result, reward, plasticity());

    return out;
}

void Creature::feed(float amount) noexcept { needs.hunger = std::clamp(needs.hunger - amount, 0.0f, 1.0f); }
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
    return child;
}

}  // namespace wsim

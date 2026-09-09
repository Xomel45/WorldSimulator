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

}  // namespace

float Creature::maxSpeed() const noexcept   { return lerpTrait(genome.trait(Genome::kSpeed), 1.0f, 6.0f); }
float Creature::size() const noexcept       { return lerpTrait(genome.trait(Genome::kSize), 0.7f, 1.4f); }
float Creature::metabolism() const noexcept { return lerpTrait(genome.trait(Genome::kMetabolism), 0.7f, 1.4f); }
float Creature::lifespan() const noexcept   { return lerpTrait(genome.trait(Genome::kLifespan), 300.0f, 1200.0f); }
float Creature::fertility() const noexcept  { return lerpTrait(genome.trait(Genome::kFertility), 0.0f, 1.0f); }

BrainOutputs Creature::tick(float dt, const SenseInputs& senses) noexcept {
    const BrainInputs in{
        .hunger  = needs.hunger,
        .energy  = needs.energy,
        .fear    = needs.fear,
        .libido  = needs.libido,
        .social  = needs.social,

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

        .nearbyAlarmLevel      = senses.nearbyAlarmLevel,
        .nearbyMatingCallLevel = senses.nearbyMatingCallLevel,

        .selfSize = size(),

        .lovedOneDirX      = senses.lovedOneDir.x,
        .lovedOneDirZ      = senses.lovedOneDir.y,
        .lovedOneProximity = senses.lovedOneProximity,
        .lovedOneThreatLevel = senses.lovedOneThreatLevel,
    };

    const BrainOutputs out = brain::evaluate(genome.brainWeights(), in);

    const float metab = metabolism();
    needs.hunger = std::clamp(needs.hunger + 0.01f * metab * dt, 0.0f, 1.0f);

    glm::vec2 moveDir{out.moveX, out.moveZ};
    if (const float len = glm::length(moveDir); len > 1.0f) moveDir /= len;
    const float moveCost = glm::length(moveDir) * 0.01f;
    needs.energy = std::clamp(needs.energy - (0.008f * metab + moveCost) * dt, 0.0f, 1.0f);

    // Страх реагирует на собственную угрозу, панику соседей (крик тревоги
    // распространяется по стае, даже если сама угроза не видна), опасность
    // близкого (вес ниже, чем у собственной угрозы, но это не декоративный
    // сигнал — вместе с defendDesire/движением он и даёт "защиту любимых") и
    // лёгкую фоновую настороженность от толпы незнакомцев (вес заметно ниже
    // настоящих угроз — эволюционировавшие веса решают, насколько конкретной
    // линии вообще есть дело до чужаков).
    const float fearLerp = std::clamp(8.0f * dt, 0.0f, 1.0f);
    const float fearTarget = std::max({
        senses.threatProximity,
        senses.nearbyAlarmLevel * 0.6f,
        senses.lovedOneThreatLevel * 0.7f,
        senses.strangerCrowding * 0.15f,
    });
    needs.fear = needs.fear + (fearTarget - needs.fear) * fearLerp;

    // Свои удовлетворяют потребность в общении сильнее чужих (но не до нуля —
    // какая-то социальная ценность есть и в компании незнакомцев).
    needs.social = std::clamp(
        needs.social + (senses.familiarCrowding * 0.06f + senses.strangerCrowding * 0.02f - 0.01f) * dt,
        0.0f, 1.0f);
    // Брачный зов соседей слегка подстёгивает либидо — социальная реплика
    // готовности к размножению, помимо собственного внутреннего дрейфа.
    needs.libido = std::clamp(
        needs.libido + (0.02f * fertility() + 0.01f * senses.nearbyMatingCallLevel) * dt, 0.0f, 1.0f);

    velocity = glm::vec3(moveDir.x, 0.0f, moveDir.y) * maxSpeed();
    position += velocity * dt;
    age += dt;
    if (age >= lifespan()) alive = false;

    return out;
}

void Creature::feed(float amount) noexcept { needs.hunger = std::clamp(needs.hunger - amount, 0.0f, 1.0f); }
void Creature::rest(float amount) noexcept { needs.energy = std::clamp(needs.energy + amount, 0.0f, 1.0f); }

void Creature::mate(std::uint64_t partnerId) noexcept {
    needs.libido = 0.0f;
    bondedPartnerId = partnerId;
}

void Creature::recordChild(std::uint64_t childId) { childIds.push_back(childId); }

Creature Creature::reproduce(const Creature& a, const Creature& b, Rng& rng) {
    Creature child;
    child.genome = Genome::crossover(a.genome, b.genome, rng);
    child.genome.mutate(rng, /*rate=*/0.05f, /*strength=*/0.15f);
    child.position = (a.position + b.position) * 0.5f;
    child.parentIds = {a.id, b.id};
    return child;
}

}  // namespace wsim

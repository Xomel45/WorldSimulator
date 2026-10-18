#include "sim/Behavior.hpp"

#include "sim/Terrain.hpp"
#include "sim/Trees.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace wsim::behavior {

namespace {

constexpr float kPi = 3.14159265f;
constexpr int   kStates = static_cast<int>(Behavior::Count);

// Множитель приоритета от выхода мозга-желания: 0.5 (нейтрально) -> 1.0, [0,1] -> [0.65, 1.35].
float gain(float desire) { return 0.65f + 0.7f * std::clamp(desire, 0.0f, 1.0f); }

glm::vec2 safeNormalize(glm::vec2 v, glm::vec2 fallback = {0.0f, 0.0f}) {
    const float len = glm::length(v);
    return len > 1.0e-5f ? v / len : fallback;
}
glm::vec2 dirTo(const glm::vec3& from, const glm::vec3& to) { return safeNormalize({to.x - from.x, to.z - from.z}); }

// Детерминированный шум 0..1 по (id, момент): постоянен на кусках по 0.5 с -- блуждание плавно "дрейфует".
float noise01(std::uint64_t id, float age) {
    std::uint64_t h = id * 0x9E3779B97F4A7C15ull + static_cast<std::uint64_t>(std::floor(age * 2.0f)) * 0xBF58476D1CE4E5B9ull;
    h ^= h >> 31; h *= 0x94D049BB133111EBull; h ^= h >> 29;
    return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0x1000000);
}

// Куда нельзя идти: вода (если мы на суше) и обрывы. Смотрим на два шага вперёд.
bool blocked(const Terrain& t, const glm::vec3& pos, glm::vec2 dir, bool inWater) {
    const float h0 = t.heightAt(pos.x, pos.z);
    for (float dist : {2.0f, 4.5f}) {
        const float x = pos.x + dir.x * dist, z = pos.z + dir.y * dist;
        if (!inWater && t.isUnderwater(x, z)) return true;
        if ((t.heightAt(x, z) - h0) / dist > 1.0f) return true;  // круче ~45 градусов -- обрыв
        if (Trees::blocksPoint(t, x, z, 0.7f)) return true;       // ствол дерева (с запасом на ширину существа)
    }
    return false;
}

// Обход препятствий: если курс упирается в воду/обрыв, пробуем отвернуть на 35, 70, 105, 140 градусов
// (с той стороны, с которой уже начали обходить -- иначе существо мечется у берега).
glm::vec2 steerAround(const Terrain* t, const glm::vec3& pos, glm::vec2 dir, bool inWater, float preferSign, bool& stuck) {
    stuck = false;
    if (!t || glm::length(dir) < 1.0e-5f) return dir;
    if (!blocked(*t, pos, dir, inWater)) return dir;
    for (int k = 1; k <= 4; ++k) {
        for (float sign : {preferSign, -preferSign}) {
            const float a = sign * static_cast<float>(k) * 35.0f * kPi / 180.0f, ca = std::cos(a), sa = std::sin(a);
            const glm::vec2 d{dir.x * ca - dir.y * sa, dir.x * sa + dir.y * ca};
            if (!blocked(*t, pos, d, inWater)) return d;
        }
    }
    stuck = true;
    return {0.0f, 0.0f};
}

}  // namespace

Decision decide(Creature& c, const SenseInputs& s, const BrainOutputs& raw, float dt) noexcept {
    const Needs& n = c.needs;
    Decision d;

    // ── Память о еде: где видели в последний раз; забывается со временем или когда пришли, а там пусто.
    const bool foodVisible = s.foodDist < kNoTarget;
    if (foodVisible) { c.foodMemory = s.foodPos; c.foodMemoryAge = 0.0f; }
    else if (c.foodMemoryAge >= 0.0f) {
        c.foodMemoryAge += dt;
        if (c.foodMemoryAge > 180.0f || glm::length(glm::vec2(c.foodMemory.x - c.position.x, c.foodMemory.z - c.position.z)) < 3.0f)
            c.foodMemoryAge = -1.0f;
    }
    const bool haveFoodTarget = foodVisible || c.foodMemoryAge >= 0.0f;
    const glm::vec3 foodTarget = foodVisible ? s.foodPos : c.foodMemory;
    const bool foodReach = foodVisible && s.foodDist <= kInteractRadius;
    const bool mateVisible = s.mateDist < kNoTarget;
    const bool groupVisible = s.groupCount > 0;

    const bool wasEating = c.behavior == Behavior::Eat, wasResting = c.behavior == Behavior::Rest;

    // ── Оценки занятий.
    std::array<float, kStates> score{};
    auto S = [&](Behavior b) -> float& { return score[static_cast<std::size_t>(b)]; };

    // Бегство: страх уже учитывает угрозу, тревогу соседей, чужой статус, дом (см. Creature::tick).
    if (n.fear > 0.4f) S(Behavior::Flee) = 3.0f * std::pow(n.fear, 0.6f) * gain(raw.fleeDesire) * (1.25f - 0.5f * c.boldness());

    // Еда: кормимся, пока голод не уйдёт (гистерезис: начали при >0.2, держимся до 0.04).
    if (foodReach && (n.hunger > 0.2f || (wasEating && n.hunger > 0.04f))) S(Behavior::Eat) = 1.5f + 2.5f * n.hunger;
    const bool hungry = n.hunger > 0.35f || (n.hunger > 0.2f && foodVisible && s.foodDist < 8.0f);
    if (hungry && !foodReach) S(Behavior::SeekFood) = 2.0f * std::pow(n.hunger, 1.2f) * gain(raw.eatDesire) * (haveFoodTarget ? 1.0f : 0.8f);

    // Отдых: устал (держимся, пока не отдохнули почти полностью).
    const float tired = 1.0f - n.energy;
    if (tired > 0.35f || (wasResting && tired > 0.1f)) S(Behavior::Rest) = 2.2f * std::pow(tired, 1.4f) * gain(raw.restDesire);
    if (c.health < 0.6f && n.hunger < 0.6f) S(Behavior::Rest) += 0.6f;  // раненому/ослабшему -- покой

    // Пара: только если готов и кого-то видит (иначе просто бродит, либидо подождёт).
    if (c.canMate() && n.libido > 0.45f && mateVisible) S(Behavior::SeekMate) = 1.6f * n.libido * gain(raw.mateDesire);

    // Общение: одинокий общительный тянется к своим, пока не пришёл.
    if (groupVisible && s.groupDist > 4.0f) S(Behavior::Socialize) = (1.0f - n.social) * c.sociability() * 0.9f;

    // Домосед: далеко от дома тянет обратно.
    if (c.awayFromHome() > 0.3f) S(Behavior::GoHome) = c.awayFromHome() * c.territorial() * 1.0f;

    // Месть: есть зуб на конкретного соседа (копится у завистливых рядом с его демонстрацией статуса). Сытый и здоровый
    // идёт к кусту, чей внешний вид знает как плохой, берёт ягоды и несёт обидчику; эмпатичные на такое не способны.
    // Выбор здесь, шаги (взять/подсунуть) выполняет мир -- см. World::tick.
    constexpr float kServeRadius = 2.2f;
    const bool carrying  = c.carryAmount > 0.0f;
    const bool targetSeen = s.grudgeDist < kNoTarget;
    const bool badSeen    = s.badFoodDist < kNoTarget;
    if (c.grudge > 0.5f && c.grudgeId != 0 && c.health > 0.5f && n.hunger < 0.6f && (carrying ? targetSeen : badSeen))
        S(Behavior::Sabotage) = 2.4f * (c.grudge - 0.25f) * std::max(0.1f, 1.3f - c.empathy());

    // Базовая линия: бродит, любопытные -- охотнее.
    S(Behavior::Wander) = 0.3f + c.curiosity() * c.boredomLevel() * 0.8f;

    // ── Выбор с гистерезисом: текущее занятие получает бонус и держится минимум 0.8 с (бегство прерывает сразу).
    c.behaviorTimer += dt;
    score[static_cast<std::size_t>(c.behavior)] += 0.35f;
    int best = 0;
    for (int i = 1; i < kStates; ++i) if (score[static_cast<std::size_t>(i)] > score[static_cast<std::size_t>(best)]) best = i;
    Behavior chosen = static_cast<Behavior>(best);
    if (chosen != c.behavior) {
        const bool urgent = chosen == Behavior::Flee || c.behavior == Behavior::Flee;
        if (!urgent && c.behaviorTimer < 0.8f && score[static_cast<std::size_t>(c.behavior)] > 0.35f) chosen = c.behavior;  // ещё не наскучило
        else c.behaviorTimer = 0.0f;
    }
    c.behavior = chosen;

    // ── Куда и с какой скоростью.
    glm::vec2 dir{0.0f};
    float     speed = 0.0f;
    auto wander = [&](float turnRate, float spd) {
        c.wanderHeading += (noise01(c.id, c.age) - 0.5f) * 2.0f * turnRate * dt;
        dir   = {std::cos(c.wanderHeading), std::sin(c.wanderHeading)};
        speed = spd;
    };
    switch (chosen) {
        case Behavior::Wander:   wander(2.2f, 0.32f); break;
        case Behavior::SeekFood:
            if (haveFoodTarget) {
                dir = dirTo(c.position, foodTarget);
                const float dist = foodVisible ? s.foodDist : glm::length(glm::vec2(foodTarget.x - c.position.x, foodTarget.z - c.position.z));
                speed = (0.55f + 0.45f * n.hunger) * std::clamp(dist / 5.0f, 0.3f, 1.0f);  // у цели замедляемся
            } else {
                wander(0.9f, 0.55f);  // еды не видно и не помним: идём исследовать, курс держим ровнее
            }
            break;
        case Behavior::Eat:      dir = dirTo(c.position, foodTarget); speed = 0.0f; d.eat = true; break;
        case Behavior::Rest:     speed = 0.0f; d.rest = true; break;
        case Behavior::Flee: {
            if (s.threatDist < kNoTarget && glm::length(s.threatDir) > 0.0f) dir = -safeNormalize(s.threatDir);
            else if (s.alarmNear > 0.2f && glm::length(s.alarmDir) > 0.0f) dir = -safeNormalize(s.alarmDir);  // паникуют -- убегаем от них
            else dir = {std::cos(c.wanderHeading), std::sin(c.wanderHeading)};
            speed = 1.0f;
            break;
        }
        case Behavior::SeekMate:
            dir = safeNormalize(s.mateDir);
            if (s.mateDist <= kInteractRadius) { speed = 0.0f; d.mate = true; }
            else speed = 0.7f * std::clamp(s.mateDist / 4.0f, 0.3f, 1.0f);
            break;
        case Behavior::Socialize: dir = safeNormalize(s.groupDir); speed = 0.5f; break;
        case Behavior::GoHome:    dir = dirTo(c.position, c.homePosition); speed = 0.5f; break;
        case Behavior::Sabotage:
            if (carrying) {
                dir = dirTo(c.position, s.grudgePos);
                if (s.grudgeDist <= kServeRadius) { speed = 0.0f; d.serve = true; }
                else speed = 0.7f * std::clamp(s.grudgeDist / 4.0f, 0.35f, 1.0f);
            } else {
                dir = dirTo(c.position, s.badFoodPos);
                if (s.badFoodDist <= kInteractRadius) { speed = 0.0f; d.pick = true; }
                else speed = 0.6f * std::clamp(s.badFoodDist / 4.0f, 0.3f, 1.0f);
            }
            break;
        case Behavior::Count:     break;
    }

    // ── Общие поправки курса: не слипаться, не уходить за край мира, личный уклон мозга.
    if (speed > 0.0f) {
        glm::vec2 v = dir;
        v += s.separation * 1.2f;
        if (s.worldHalfSize > 0.0f) {
            constexpr float margin = 14.0f;
            const float lim = s.worldHalfSize - margin;
            if (std::fabs(c.position.x) > lim) v.x -= std::copysign(std::min((std::fabs(c.position.x) - lim) / margin, 1.5f), c.position.x) * 1.5f;
            if (std::fabs(c.position.z) > lim) v.y -= std::copysign(std::min((std::fabs(c.position.z) - lim) / margin, 1.5f), c.position.z) * 1.5f;
        }
        v += glm::vec2(raw.moveX, raw.moveZ) * 0.25f;
        v = safeNormalize(v, dir);

        // Обход воды и обрывов: выбираем сторону обхода по "чётности" существа и держим её.
        const bool inWater = s.terrain && s.terrain->isUnderwater(c.position.x, c.position.z);
        bool stuck = false;
        v = steerAround(s.terrain, c.position, v, inWater, (c.id & 1) ? 1.0f : -1.0f, stuck);
        if (stuck) {                       // упёрлись: сменим курс блуждания и постоим тик
            c.wanderHeading += kPi * 0.5f;
            speed = 0.0f;
        }
        d.move = v * speed;
    } else {
        d.move = {0.0f, 0.0f};
    }

    // ── Сигналы наружу: паника слышна, брачный зов -- у готовых искать пару.
    d.alarm      = n.fear > 0.5f ? n.fear : 0.0f;
    d.matingCall = chosen == Behavior::SeekMate ? 0.85f : (c.canMate() ? n.libido * 0.5f : 0.0f);
    return d;
}

}  // namespace wsim::behavior

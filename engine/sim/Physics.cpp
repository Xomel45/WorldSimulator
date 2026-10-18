#include "sim/Physics.hpp"

#include <algorithm>
#include <cmath>

namespace wsim {

namespace {

constexpr float kPosCorrect   = 0.8f;    // доля коррекции проникновения за шаг
constexpr float kPenSlop      = 0.005f;  // допуск проникновения
constexpr float kRollResist   = 0.14f;   // трение качения: замедление = kRollResist * g * cos(угла); уклон < ~8 градусов останавливает валун
constexpr float kAirDrag      = 0.02f;
constexpr float kWaterDrag    = 3.2f;    // вода быстро гасит скорость

// Нормаль рельефа в точке (центральные разности heightAt по 0.5 юнита).
glm::vec3 terrainNormal(const Terrain& t, float x, float z) {
    constexpr float e = 0.5f;
    const float dx = (t.heightAt(x + e, z) - t.heightAt(x - e, z)) / (2.0f * e);
    const float dz = (t.heightAt(x, z + e) - t.heightAt(x, z - e)) / (2.0f * e);
    return glm::normalize(glm::vec3(-dx, 1.0f, -dz));
}

}  // namespace

int PhysicsWorld::addSphere(const glm::vec3& pos, float radius, float density, const glm::vec3& vel) {
    if (m_bodies.size() >= kMaxBodies) m_bodies.erase(m_bodies.begin());  // самый старый
    RigidBody b;
    b.pos     = pos;
    b.vel     = vel;
    b.radius  = std::max(radius, 0.2f);
    b.invMass = 1.0f / std::max(b.radius * b.radius * b.radius * density, 1.0e-3f);
    m_bodies.push_back(b);
    return static_cast<int>(m_bodies.size()) - 1;
}

// Шар против поверхности-функции: расстояние до плоскости касательной ~ (y - h) * n.y.
// Выталкиваем по нормали, гасим скорость в неё (с отскоком, если удар сильный),
// трение качения гасит скорость вдоль поверхности, а качение крутит ориентацию.
void PhysicsWorld::collideTerrain(RigidBody& b, const Terrain& terrain, float dt) {
    const float h = terrain.heightAt(b.pos.x, b.pos.z);
    const glm::vec3 n = terrainNormal(terrain, b.pos.x, b.pos.z);
    const float depth = b.radius - (b.pos.y - h) * n.y;
    if (depth <= 0.0f) return;

    b.pos += n * std::max(depth - kPenSlop, 0.0f) * kPosCorrect;

    const float vn = glm::dot(b.vel, n);
    if (vn < 0.0f) {
        if (-vn > kImpactSpeed) m_impacts.push_back({b.pos, -vn / b.invMass, b.radius});
        const float e = -vn > 2.5f ? b.restitution : 0.0f;  // мелкие касания не подпрыгивают
        b.vel -= n * ((1.0f + e) * vn);
    }

    // Качение: скорость вдоль поверхности медленно гаснет, ориентация крутится вокруг оси n x v.
    glm::vec3 vt = b.vel - n * glm::dot(b.vel, n);
    const float speed = glm::length(vt);
    if (speed > 1.0e-3f) {
        b.orient = glm::normalize(glm::angleAxis(speed / b.radius * dt, glm::normalize(glm::cross(n, vt))) * b.orient);
        b.vel -= (vt / speed) * std::min(kRollResist * kGravity * n.y * dt, speed);
    }
}

void PhysicsWorld::collidePair(RigidBody& a, RigidBody& b) {
    const glm::vec3 d = b.pos - a.pos;
    const float     dist = glm::length(d);
    const float     minDist = a.radius + b.radius;
    if (dist >= minDist || dist < 1.0e-5f) return;

    const glm::vec3 n = d / dist;
    const float     invSum = a.invMass + b.invMass;
    const float     corr = std::max(minDist - dist - kPenSlop, 0.0f) * kPosCorrect / invSum;
    a.pos -= n * (corr * a.invMass);
    b.pos += n * (corr * b.invMass);

    const float velN = glm::dot(b.vel - a.vel, n);
    if (velN > 0.0f) return;  // уже расходятся
    const float e = std::min(a.restitution, b.restitution);
    const float j = -(1.0f + e) * velN / invSum;
    a.vel -= n * (j * a.invMass);
    b.vel += n * (j * b.invMass);
    a.asleep = b.asleep = false;
}

void PhysicsWorld::step(float dt, const Terrain& terrain) {
    m_impacts.clear();

    for (auto& b : m_bodies) {
        if (b.asleep) {
            b.asleepFor += dt;
            // Рельеф под спящим изменили (подняли -- вдавило, опустили -- завис): просыпаемся.
            const float h = terrain.heightAt(b.pos.x, b.pos.z);
            const glm::vec3 n = terrainNormal(terrain, b.pos.x, b.pos.z);
            const float gap = (b.pos.y - h) * n.y - b.radius;
            if (gap > 0.15f || gap < -0.3f) { b.asleep = false; b.sleepTimer = 0.0f; b.asleepFor = 0.0f; }
            continue;
        }

        b.vel.y -= kGravity * dt;
        const float drag = terrain.isUnderwater(b.pos.x, b.pos.z) ? kWaterDrag : kAirDrag;
        b.vel *= std::exp(-drag * dt);
        b.pos += b.vel * dt;

        collideTerrain(b, terrain, dt);

        if (glm::length(b.vel) < kSleepSpeed) {
            b.sleepTimer += dt;
            if (b.sleepTimer > kSleepDelay) { b.asleep = true; b.vel = glm::vec3(0.0f); b.asleepFor = 0.0f; }
        } else {
            b.sleepTimer = 0.0f;
        }
    }

    // Шары друг о друга (их немного -- O(n^2) без сетки).
    for (std::size_t i = 0; i < m_bodies.size(); ++i)
        for (std::size_t j = i + 1; j < m_bodies.size(); ++j) {
            if (m_bodies[i].asleep && m_bodies[j].asleep) continue;
            collidePair(m_bodies[i], m_bodies[j]);
        }

    std::erase_if(m_bodies, [](const RigidBody& b) { return b.asleep && b.asleepFor > kRemoveAfterAsleep; });
}

}  // namespace wsim

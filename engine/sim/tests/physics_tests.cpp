#include "sim/Physics.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>

using namespace wsim;

namespace {
constexpr float kDt = 1.0f / 60.0f;
void run(PhysicsWorld& w, const Terrain& t, int steps) {
    for (int i = 0; i < steps; ++i) w.step(kDt, t);
}

// Первая точка суши (высота заметно выше уровня моря) на сетке -- тест не зависит от того, где вода у этого сида.
glm::vec2 findLand(const Terrain& t) {
    for (float x = -80.0f; x <= 80.0f; x += 7.0f)
        for (float z = -80.0f; z <= 80.0f; z += 7.0f)
            if (t.heightAt(x, z) > 1.0f && t.heightAt(x + 3, z) > 1.0f && t.heightAt(x, z + 3) > 1.0f) return {x, z};
    return {0.0f, 0.0f};
}
}  // namespace

int main() {
    Terrain terrain(100.0f, 3);

    // Падение: шар ложится на поверхность (центр на радиус над землёй вдоль нормали), засыпает.
    {
        PhysicsWorld w;
        const glm::vec2 land = findLand(terrain);
        const float x = land.x, z = land.y;
        w.addSphere({x, terrain.heightAt(x, z) + 25.0f, z}, 1.5f);
        bool impact = false;
        for (int i = 0; i < 60 * 8; ++i) {
            w.step(kDt, terrain);
            if (!w.impacts().empty()) { impact = true; WSIM_CHECK(w.impacts()[0].impulse > 0.0f); }
        }
        WSIM_CHECK(impact);  // падение с 25 юнитов -- это удар
        WSIM_CHECK(w.bodies().size() == 1);
        const RigidBody& b = w.bodies()[0];
        const float ground = terrain.heightAt(b.pos.x, b.pos.z);
        WSIM_CHECK(b.pos.y >= ground + 1.0f && b.pos.y <= ground + 1.5f / 0.9f + 0.3f);
        WSIM_CHECK(std::isfinite(b.pos.x) && std::isfinite(b.pos.y));
        WSIM_CHECK(b.asleep);
    }

    // Склон: шар на крутом холме скатывается вниз и успокаивается ниже старта.
    {
        Terrain hill(100.0f, 3);
        for (int i = 0; i < 22; ++i) hill.raise(0.0f, 0.0f, 18.0f, 1.0f);
        PhysicsWorld w;
        const float sx = 8.0f, sz = 0.0f;
        const float startH = hill.heightAt(sx, sz);
        w.addSphere({sx, startH + 1.5f, sz}, 1.2f);
        run(w, hill, 60 * 12);
        const RigidBody& b = w.bodies()[0];
        WSIM_CHECK(b.pos.y < startH - 2.0f);                 // скатился заметно ниже
        WSIM_CHECK(std::hypot(b.pos.x, b.pos.z) > std::hypot(sx, sz) + 2.0f);  // и дальше от вершины
        WSIM_CHECK(b.asleep);
    }

    // Два шара друг в друге расталкиваются; в сумме импульс ненулевой.
    {
        PhysicsWorld w;
        const glm::vec2 land2 = findLand(terrain);
        const float x = land2.x, z = land2.y, h = terrain.heightAt(x, z);
        w.addSphere({x, h + 30.0f, z}, 1.0f);
        w.addSphere({x + 0.4f, h + 30.0f, z}, 1.0f);
        run(w, terrain, 20);
        WSIM_CHECK(glm::length(w.bodies()[0].pos - w.bodies()[1].pos) > 1.5f);
    }

    // Лимит тел: самый старый уходит.
    {
        PhysicsWorld w;
        for (std::size_t i = 0; i < PhysicsWorld::kMaxBodies + 10; ++i) w.addSphere({float(i), 50.0f, 0.0f}, 0.5f);
        WSIM_CHECK(w.bodies().size() == PhysicsWorld::kMaxBodies);
    }

    // Спящий просыпается, когда рельеф под ним поднимают (и когда опускают).
    {
        Terrain t(100.0f, 9);
        PhysicsWorld w;
        const glm::vec2 land3 = findLand(t);
        const float x = land3.x, z = land3.y;
        w.addSphere({x, t.heightAt(x, z) + 1.0f, z}, 1.0f);
        run(w, t, 60 * 6);
        WSIM_CHECK(w.bodies()[0].asleep);
        t.raise(x, z, 8.0f, 4.0f);
        w.step(kDt, t);
        WSIM_CHECK(!w.bodies()[0].asleep);
        run(w, t, 60 * 8);
        const RigidBody& rb = w.bodies()[0];
        WSIM_CHECK(rb.pos.y > t.heightAt(rb.pos.x, rb.pos.z) + 0.5f);  // вытолкнут на новую поверхность, не провалился
    }

    // Очистка.
    {
        PhysicsWorld w;
        w.addSphere({0, 10, 0}, 1.0f);
        w.clear();
        WSIM_CHECK(w.bodies().empty());
    }

    std::puts("physics_tests: OK");
    return 0;
}

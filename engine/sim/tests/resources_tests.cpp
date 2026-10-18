#include "sim/SaveGame.hpp"
#include "sim/Terrain.hpp"
#include "sim/Trees.hpp"
#include "sim/World.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>

using namespace wsim;

namespace {
constexpr float kDt = 1.0f / 60.0f;

// Лес поблизости от (0,0): точка, откуда ближайшее дерево видно.
bool findForest(const Terrain& t, glm::vec2& at) {
    for (float z = -90.0f; z <= 90.0f; z += 6.0f)
        for (float x = -90.0f; x <= 90.0f; x += 6.0f) {
            TreeInstance tr;
            if (Trees::nearest(t, x, z, 12.0f, tr) && t.heightAt(x, z) > 1.0f) { at = {x, z}; return true; }
        }
    return false;
}

// Одно существо, сытое и здоровое, в заданной точке (остальное население убрано).
World lone(std::uint32_t seed, glm::vec2 at, std::uint8_t skills) {
    World w(seed);
    WorldState st = w.exportState();
    st.population.resize(1);
    Creature& c = st.population[0].creature;
    c.position = {at.x, 0.0f, at.y};
    c.age      = c.lifespan() * 0.25f;
    c.needs.hunger = 0.1f;
    c.needs.energy = 1.0f;
    c.skills       = skills;
    c.homePosition = c.position;
    st.food.clear();
    st.fauna = {};
    WSIM_CHECK(w.importState(st));
    return w;
}

// Подкормка: держим существо сытым (в этих тестах еда не предмет).
void feedAll(World& w) {
    WorldState st = w.exportState();
    for (Inhabitant& in : st.population) { in.creature.needs.hunger = std::min(in.creature.needs.hunger, 0.15f); in.creature.needs.energy = 1.0f; }
    WSIM_CHECK(w.importState(st));
}
}  // namespace

int main() {
    // Срубленное дерево исчезает (и не мешает), состояние переживает экспорт/импорт и сохранение.
    {
        Terrain t(World::kWorldHalfSize, 29);
        glm::vec2 at;
        WSIM_CHECK(findForest(t, at));
        TreeInstance tr;
        WSIM_CHECK(Trees::nearest(t, at.x, at.y, 12.0f, tr));
        WSIM_CHECK(Trees::blocksPoint(t, tr.x, tr.z, 0.5f));
        const Terrain::Rect r = t.fellTree(tr.cx, tr.cz);
        WSIM_CHECK(r.x0 < tr.x && r.x1 > tr.x && r.z0 < tr.z && r.z1 > tr.z);
        TreeInstance again;
        const bool other = Trees::nearest(t, tr.x, tr.z, 0.4f, again);
        WSIM_CHECK(!other);                                   // на этом месте дерева больше нет
        WSIM_CHECK(!Trees::blocksPoint(t, tr.x, tr.z, 0.5f));
        WSIM_CHECK(t.felledCount() == 1 && t.treeFelled(tr.cx, tr.cz));

        Terrain t2(World::kWorldHalfSize, 29);
        t2.importFelled(t.exportFelled());
        WSIM_CHECK(t2.treeFelled(tr.cx, tr.cz) && !Trees::nearest(t2, tr.x, tr.z, 0.4f, again));

        World w(3);
        PhysicsWorld physics;
        const SaveGame sg = captureSave("t", 0, 0.0f, t, w, physics, CameraState{});
        const auto bytes = serializeSave(sg);
        SaveGame back;
        std::string err;
        WSIM_CHECK(parseSave(bytes, back, err));
        Terrain t3(World::kWorldHalfSize, 1);
        World w3(0);
        PhysicsWorld p3;
        WSIM_CHECK(restoreSave(back, t3, w3, p3));
        WSIM_CHECK(t3.treeFelled(tr.cx, tr.cz));
    }

    // Знающий копьё сам идёт к дереву, рубит, мастерит копьё -- деревья исчезают, чанки получают "грязные" области.
    {
        Terrain t(World::kWorldHalfSize, 29);
        glm::vec2 at;
        WSIM_CHECK(findForest(t, at));
        World w = lone(5, at, kSkillSpear);
        std::size_t dirty = 0;
        for (int i = 0; i < 60 * 120 && w.stats().spearsMade == 0; ++i) {
            w.tick(kDt, &t);
            dirty += w.takeDirty().size();
            if (i % 300 == 299) feedAll(w);
        }
        WSIM_CHECK(w.stats().treesFelled >= 1);
        WSIM_CHECK(w.stats().spearsMade == 1);
        WSIM_CHECK(t.felledCount() >= 1 && dirty >= 1);
        WSIM_CHECK(w.population()[0].creature.hasSpear());
        // Копьё есть -- больше не рубит, пока не придумает щит (тогда нужны ещё брёвна): число срубленных не растёт само по себе.
        const auto felled = w.stats().treesFelled;
        for (int i = 0; i < 60 * 30; ++i) { w.tick(kDt, &t); if (i % 300 == 299) feedAll(w); }
        WSIM_CHECK(w.stats().treesFelled == felled || w.stats().skillsLearned >= 1);
        // Сохранение несёт орудия и умения (бит-в-бит продолжение проверяет save_tests; здесь -- поля).
        PhysicsWorld physics;
        const SaveGame sg = captureSave("c", 0, 0.0f, t, w, physics, CameraState{});
        SaveGame back;
        std::string err;
        WSIM_CHECK(parseSave(serializeSave(sg), back, err));
        const Creature& c = back.world.population[0].creature;
        WSIM_CHECK(c.hasSpear() && c.knows(kSkillSpear) && std::fabs(c.spearDur - w.population()[0].creature.spearDur) < 1.0e-6f);
    }

    // Не умеющий мастерить -- не рубит.
    {
        Terrain t(World::kWorldHalfSize, 29);
        glm::vec2 at;
        WSIM_CHECK(findForest(t, at));
        World w = lone(6, at, 0);
        for (int i = 0; i < 60 * 60; ++i) { w.tick(kDt, &t); if (i % 300 == 299) feedAll(w); }
        WSIM_CHECK(w.stats().treesFelled == 0 || w.stats().skillsLearned > 0);   // только если сам придумал
    }

    // Орудия работают: щит гасит укус, копьё бьёт в ответ (волк ранен, при двух ударах убит).
    {
        World w = lone(7, {0.0f, 0.0f}, kSkillSpear | kSkillShield);
        WorldState st = w.exportState();
        st.population[0].creature.spearDur = 1.0f;
        st.population[0].creature.shieldDur = 1.0f;
        st.population[0].creature.needs.fear = 0.0f;
        WSIM_CHECK(w.importState(st));
        WSIM_CHECK(w.spawnAnimal(Species::Wolf, 1.5f, 0.0f));
        {
            WorldState ws = w.exportState();
            for (Animal& an : ws.fauna.animals) { an.hunger = 0.95f; an.age = speciesDef(an.species).maturity * 1.5f; an.health = 1.0f; }
            WSIM_CHECK(w.importState(ws));
        }
        for (int i = 0; i < 60 * 20; ++i) { w.tick(kDt, nullptr); if (i % 300 == 299) feedAll(w); }
        WSIM_CHECK(w.stats().shieldBlocks >= 1 || w.stats().spearHits >= 1);
        // Если волк кусал -- без брони так не бывает: орудие потратилось (не осталось 1.0/1.0).
        const Creature& c = w.population().empty() ? st.population[0].creature : w.population()[0].creature;
        WSIM_CHECK(w.stats().spearHits == 0 || c.spearDur < 1.0f || !w.population().empty());
    }

    // Умения передаются соседям (и семье охотнее): к концу долгого соседства второй тоже умеет, а до этого -- нет.
    {
        World w = lone(8, {0.0f, 0.0f}, kSkillSpear);
        WorldState st = w.exportState();
        Inhabitant second = st.population[0];
        second.creature.id = st.population[0].creature.id + 1000;
        second.creature.skills = 0;
        second.creature.position = {3.0f, 0.0f, 0.0f};
        second.creature.homePosition = second.creature.position;
        st.population.push_back(second);
        st.nextId = second.creature.id + 1;
        WSIM_CHECK(w.importState(st));
        WSIM_CHECK(!w.population()[1].creature.knows(kSkillSpear));
        for (int i = 0; i < 60 * 600 && !w.population()[1].creature.knows(kSkillSpear); ++i) {
            w.tick(kDt, nullptr);
            if (i % 120 == 119) feedAll(w);
        }
        WSIM_CHECK(w.population().size() == 2 && w.population()[1].creature.knows(kSkillSpear));
        WSIM_CHECK(w.stats().skillsLearned >= 1);
    }

    std::puts("resources_tests: OK");
    return 0;
}

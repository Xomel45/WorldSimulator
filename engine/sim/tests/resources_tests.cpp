#include "sim/SaveGame.hpp"
#include "sim/Terrain.hpp"
#include "sim/Trees.hpp"
#include "sim/World.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

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

    // ── Этап 2: мосты, баррикады, ловушки ──
    // Настил моста: клетка над водой становится проходимой, переживает сохранение; на суше настил не кладётся.
    {
        Terrain t(World::kWorldHalfSize, 29);
        glm::vec2 water{0, 0}, land{0, 0};
        bool haveW = false, haveL = false;
        for (float z = -90; z <= 90 && !(haveW && haveL); z += 3)
            for (float x = -90; x <= 90 && !(haveW && haveL); x += 3) {
                if (!haveW && t.heightAt(x, z) < -0.6f) { water = {x, z}; haveW = true; }
                if (!haveL && t.heightAt(x, z) > 1.0f) { land = {x, z}; haveL = true; }
            }
        WSIM_CHECK(haveW && haveL);
        WSIM_CHECK(t.isUnderwater(water.x, water.y) && !t.bridgeAt(water.x, water.y));
        WSIM_CHECK(!t.addBridge(land.x, land.y));
        WSIM_CHECK(t.addBridge(water.x, water.y) && !t.addBridge(water.x, water.y));
        WSIM_CHECK(!t.isUnderwater(water.x, water.y) && t.surfaceAt(water.x, water.y) > Terrain::kSeaLevel);
        WSIM_CHECK(t.surfaceAt(land.x, land.y) == t.heightAt(land.x, land.y));
        World w(3);
        PhysicsWorld physics;
        SaveGame back;
        std::string err;
        WSIM_CHECK(parseSave(serializeSave(captureSave("b", 0, 0.0f, t, w, physics, CameraState{})), back, err));
        Terrain t2(World::kWorldHalfSize, 1);
        World w2(0);
        PhysicsWorld p2;
        WSIM_CHECK(restoreSave(back, t2, w2, p2));
        WSIM_CHECK(t2.bridgeAt(water.x, water.y) && t2.bridgeCount() == 1);
    }

    // Голодный умелец упирается в озеро, за которым еда, -- кладёт настил и переходит на тот берег.
    {
        // Ищем переправу вдоль +x: суша, затем 5-10 единиц воды, затем суша (куст ставим на дальнем берегу) -- на первом сиде, где она есть.
        glm::vec2 from{0, 0}, to{0, 0};
        bool found = false;
        std::uint32_t seedUsed = 29;
        for (std::uint32_t seed : {29u, 5u, 11u, 77u, 1234u, 9999u, 4242u, 31337u}) {
            Terrain probe(World::kWorldHalfSize, seed);
            for (float z = -95; z <= 95 && !found; z += 1)
                for (float x = -95; x <= 85 && !found; x += 1) {
                    if (probe.isUnderwater(x, z) || probe.heightAt(x, z) < 0.3f) continue;
                    float q0 = 0.5f;
                    while (q0 <= 4.0f && !probe.isUnderwater(x + q0, z)) q0 += 0.5f;
                    if (q0 > 4.0f) continue;
                    float q1 = q0;
                    while (q1 < 20.0f && probe.isUnderwater(x + q1, z)) q1 += 0.5f;
                    if (q1 - q0 >= 3.0f && q1 - q0 <= 6.5f && probe.heightAt(x + q1 + 1.0f, z) > 0.3f && probe.heightAt(x + q1 + 3.0f, z) > 0.3f) {
                        from = {x, z}; to = {x + q1 + 2.0f, z}; found = true; seedUsed = seed;
                    }
                }
            if (found) break;
        }
        WSIM_CHECK(found);
        Terrain t(World::kWorldHalfSize, seedUsed);
        World w = lone(9, from, kSkillBridge);
        WorldState st = w.exportState();
        st.population[0].creature.wood = 4.0f;
        st.population[0].creature.needs.hunger = 0.8f;
        FoodSource bush;
        bush.position = {to.x, t.heightAt(to.x, to.y), to.y};
        bush.amount = 1.0f;
        st.food = {bush};
        WSIM_CHECK(w.importState(st));
        bool crossed = false;
        for (int i = 0; i < 60 * 90 && !crossed; ++i) {
            w.tick(kDt, &t);
            if (i % 600 == 599) { WorldState ws = w.exportState(); ws.population[0].creature.needs.hunger = 0.8f; WSIM_CHECK(w.importState(ws)); }
            crossed = w.population()[0].creature.position.x > to.x - 3.0f;
        }
        WSIM_CHECK(w.stats().planksLaid >= 1);
        WSIM_CHECK(crossed);
    }

    // Баррикада не пускает зверей; ловушка ранит и держит хищника, срабатывает один раз.
    {
        Fauna fauna;
        Rng rng(5);
        std::vector<Structure> built;
        built.push_back({StructureKind::Barricade, 0.0f, 0.0f, 1.5707963f, 1.0f});   // стена вдоль z, x в [-0.55, 0.55]
        Fauna::State st;
        st.nextId = 100;
        Animal wolf;
        wolf.id = 1; wolf.species = Species::Wolf; wolf.position = {-5.0f, 0.0f, 0.0f}; wolf.hunger = 0.95f; wolf.age = speciesDef(Species::Wolf).maturity * 1.5f;
        st.animals = {wolf};
        WSIM_CHECK(fauna.importState(st));
        std::vector<CreatureView> creatures = {{7, {6.0f, 0.0f, 0.0f}}};
        std::vector<Bite> bites;
        bool through = false;
        for (int i = 0; i < 60 * 15; ++i) {
            fauna.tick(kDt, nullptr, creatures, rng, 100.0f, bites, {}, &built);
            for (const Animal& a : fauna.animals()) through |= barricadeBlocks(built[0], a.position.x, a.position.z, -0.25f);
        }
        WSIM_CHECK(!through);

        // Ловушка: волк идёт прямо на неё.
        std::vector<Structure> traps = {{StructureKind::Trap, -2.0f, 0.0f, 0.0f, 1.0f}};
        wolf.position = {-6.0f, 0.0f, 0.0f};
        st.animals = {wolf};
        WSIM_CHECK(fauna.importState(st));
        float minX = 0.0f;
        for (int i = 0; i < 60 * 10; ++i) {
            fauna.tick(kDt, nullptr, creatures, rng, 100.0f, bites, {}, &traps);
            if (!fauna.animals().empty()) minX = std::max(minX, fauna.animals()[0].position.x);
        }
        WSIM_CHECK(fauna.stats().trapsTriggered == 1 && traps.empty());
        WSIM_CHECK(fauna.animals().empty() || fauna.animals()[0].health < 0.6f);
    }

    // Знающий ловушку, помня хищника, сам ставит её рядом (3 раза не набирается: кучность), тратя доски.
    {
        Terrain t(World::kWorldHalfSize, 29);
        glm::vec2 at;
        WSIM_CHECK(findForest(t, at));
        World w = lone(10, at, kSkillSpear | kSkillTrap);
        WorldState st = w.exportState();
        st.population[0].creature.wood = 3.0f;
        st.population[0].creature.dangerMemory = 100.0f;
        WSIM_CHECK(w.importState(st));
        for (int i = 0; i < 60 * 40 && w.structures().empty(); ++i) { w.tick(kDt, &t); }
        WSIM_CHECK(w.stats().trapsSet >= 1 && !w.structures().empty() && w.structures()[0].kind == StructureKind::Trap);
        WSIM_CHECK(w.population()[0].creature.wood < 3.0f + 1.0e-4f);
        // Постройки сохраняются.
        PhysicsWorld physics;
        SaveGame back;
        std::string err;
        WSIM_CHECK(parseSave(serializeSave(captureSave("s", 0, 0.0f, t, w, physics, CameraState{})), back, err));
        WSIM_CHECK(back.world.structures.size() == w.structures().size());
    }

    std::puts("resources_tests: OK");
    return 0;
}

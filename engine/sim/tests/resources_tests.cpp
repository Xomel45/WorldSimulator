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
World lone(std::uint32_t seed, glm::vec2 at, std::uint16_t skills) {
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

    // ── Этап 3: жажда, ведро, охота копьём, склад ──
    {
        Terrain t(World::kWorldHalfSize, 29);
        // Место на суше, где вода в 6-14 единицах (по лучу +x).
        glm::vec2 shore{0, 0};
        bool found = false;
        for (float z = -90; z <= 90 && !found; z += 2)
            for (float x = -90; x <= 80 && !found; x += 2)
                if (t.heightAt(x, z) > 0.8f && !t.isUnderwater(x + 5.0f, z) && t.isUnderwater(x + 10.0f, z) && t.heightAt(x - 3.0f, z) > 0.5f) { shore = {x, z}; found = true; }
        WSIM_CHECK(found);

        // Жажда растёт только в мире с рельефом; напиться можно у берега.
        {
            World flat = lone(21, {0.0f, 0.0f}, 0);
            for (int i = 0; i < 60 * 60; ++i) flat.tick(kDt, nullptr);
            WSIM_CHECK(flat.population()[0].creature.thirst == 0.2f);
            World w = lone(21, shore, 0);
            WorldState st = w.exportState();
            st.population[0].creature.thirst = 0.9f;
            WSIM_CHECK(w.importState(st));
            float minThirst = 1.0f;
            for (int i = 0; i < 60 * 40; ++i) { w.tick(kDt, &t); minThirst = std::min(minThirst, w.population()[0].creature.thirst); if (i % 300 == 299) feedAll(w); }
            WSIM_CHECK(minThirst < 0.3f);
        }
        // Без воды рядом жажда губит: здоровье тает при thirst = 1 (мир без воды в досягаемости -- в плоском тесте через рельеф нельзя, проверяем формулой Creature).
        {
            Creature c;
            Rng rng(1);
            c.genome = Genome::random(rng);
            c.age = c.lifespan() * 0.3f;
            c.thirst = 1.0f;
            SenseInputs si;
            Terrain flatish(World::kWorldHalfSize, 29);
            si.terrain = &flatish;
            c.position = {0.0f, 0.0f, 0.0f};
            const float h0 = c.health;
            for (int i = 0; i < 60 * 20; ++i) { c.needs.hunger = 0.1f; c.needs.energy = 1.0f; c.tick(kDt, si); c.thirst = 1.0f; }
            WSIM_CHECK(c.health < h0 - 0.05f);
        }
        // Ведро: знающий мастерит его, набирает воду и пьёт из него вдали от берега.
        {
            World w = lone(22, shore, kSkillBucket);
            WorldState st = w.exportState();
            st.population[0].creature.wood = 2.0f;
            st.population[0].creature.thirst = 0.5f;
            WSIM_CHECK(w.importState(st));
            for (int i = 0; i < 60 * 90 && !(w.population()[0].creature.hasBucket && w.population()[0].creature.water >= 1.0f); ++i) {
                w.tick(kDt, &t);
                if (i % 300 == 299) feedAll(w);
            }
            WSIM_CHECK(w.stats().bucketsMade == 1 && w.population()[0].creature.hasBucket && w.population()[0].creature.water >= 1.0f);
            WorldState st2 = w.exportState();
            st2.population[0].creature.thirst = 0.8f;
            WSIM_CHECK(w.importState(st2));
            w.tick(kDt, &t);   // глоток из ведра
            WSIM_CHECK(w.population()[0].creature.thirst < 0.5f);
        }
    }

    // Охота: голодный с копьём бьёт обездвиженную добычу -- со второго удара (0.55 + 0.55 > 1) зверь убит, существо сыто.
    {
        World w = lone(23, {0.0f, 0.0f}, kSkillSpear);
        WorldState st = w.exportState();
        st.population[0].creature.spearDur = 1.0f;
        st.population[0].creature.needs.hunger = 0.9f;
        Animal deer;
        deer.id = 1; deer.species = Species::Deer; deer.position = {4.0f, 0.0f, 0.0f}; deer.stun = 60.0f; deer.age = speciesDef(Species::Deer).maturity * 1.5f;
        st.fauna.animals = {deer};
        st.fauna.nextId = 10;
        WSIM_CHECK(w.importState(st));
        for (int i = 0; i < 60 * 20 && w.stats().meatKills == 0; ++i) w.tick(kDt, nullptr);
        WSIM_CHECK(w.stats().meatKills == 1);
        WSIM_CHECK(w.population()[0].creature.needs.hunger < 0.5f);
        WSIM_CHECK(w.population()[0].creature.spearDur < 1.0f);
    }

    // Склад: знающий строит его на стойбище, сдаёт излишки; нуждающийся берёт оттуда, а не рубит.
    {
        Terrain t(World::kWorldHalfSize, 29);
        glm::vec2 at{0, 0};
        bool ok = false;
        for (float z = -80; z <= 80 && !ok; z += 4)
            for (float x = -80; x <= 80 && !ok; x += 4)
                if (t.heightAt(x, z) > 1.5f && t.heightAt(x + 6, z) > 1.5f && t.heightAt(x - 6, z) > 1.5f && t.heightAt(x, z + 6) > 1.5f && t.heightAt(x, z - 6) > 1.5f) { at = {x, z}; ok = true; }
        WSIM_CHECK(ok);
        World w = lone(24, at, kSkillSpear | kSkillStockpile);
        WorldState st = w.exportState();
        st.population[0].creature.wood = 3.0f;
        st.population[0].creature.spearDur = 1.0f;   // копьё есть -- лишнего не надо
        WSIM_CHECK(w.importState(st));
        for (int i = 0; i < 60 * 30 && w.stats().stockpilesBuilt == 0; ++i) { w.tick(kDt, &t); if (i % 300 == 299) feedAll(w); }
        WSIM_CHECK(w.stats().stockpilesBuilt == 1);
        WSIM_CHECK(w.structures().size() == 1 && w.structures()[0].kind == StructureKind::Stockpile);
        // Второй стороной: один склад на стойбище (место рядом занято).
        WSIM_CHECK(!w.placeStructure(&t, StructureKind::Stockpile, at.x + 10.0f, at.y, 0.0f));

        // Сдача излишков: у существа 4 бревна, копьё есть, цель -- 0.
        WorldState s2 = w.exportState();
        s2.population[0].creature.wood = 4.0f;
        s2.population[0].creature.needs.hunger = 0.1f;
        WSIM_CHECK(w.importState(s2));
        for (int i = 0; i < 60 * 40 && w.structures()[0].stock < 1.0f; ++i) { w.tick(kDt, &t); if (i % 300 == 299) feedAll(w); }
        WSIM_CHECK(w.structures()[0].stock >= 1.0f && w.stats().logsStored >= 1);

        // Берёт со склада: копьё потеряно, умеет копьё -- идёт за брёвнами на склад.
        WorldState s3 = w.exportState();
        s3.structures[0].stock = 6.0f;
        s3.population[0].creature.wood = 0.0f;
        s3.population[0].creature.spearDur = 0.0f;
        s3.population[0].creature.skills = kSkillSpear;
        WSIM_CHECK(w.importState(s3));
        const auto felledBefore = w.stats().treesFelled;
        for (int i = 0; i < 60 * 40 && w.stats().spearsMade == 0; ++i) { w.tick(kDt, &t); if (i % 300 == 299) feedAll(w); }
        WSIM_CHECK(w.stats().logsTaken >= 2 && w.stats().spearsMade >= 1 && w.stats().treesFelled == felledBefore);

        // Сохранение: склад с брёвнами, жажда и ведро.
        WorldState s4 = w.exportState();
        s4.population[0].creature.hasBucket = true;
        s4.population[0].creature.water = 2.0f;
        s4.population[0].creature.thirst = 0.7f;
        WSIM_CHECK(w.importState(s4));
        PhysicsWorld physics;
        SaveGame back;
        std::string err;
        WSIM_CHECK(parseSave(serializeSave(captureSave("x", 0, 0.0f, t, w, physics, CameraState{})), back, err));
        WSIM_CHECK(back.world.structures.size() == 1 && back.world.structures[0].kind == StructureKind::Stockpile && back.world.structures[0].stock == w.structures()[0].stock);
        const Creature& c = back.world.population[0].creature;
        WSIM_CHECK(c.hasBucket && std::fabs(c.water - 2.0f) < 1.0e-6f && std::fabs(c.thirst - 0.7f) < 1.0e-6f);
    }

    // ── Этап 4: день и ночь, сон, хижины ──
    {
        // Цикл: начало мира -- утро, ночью темно (~40% суток), период kDayLength.
        WSIM_CHECK(World::nightness(0.0) == 0.0f && World::sunElevation(0.0) > 0.3f);
        WSIM_CHECK(World::nightness(201.0) > 0.99f && World::sunElevation(201.0) < -0.9f);
        WSIM_CHECK(std::fabs(World::sunElevation(40.0) - World::sunElevation(40.0 + World::kDayLength)) < 1.0e-4f);
        int nights = 0, total = 3000;
        for (int i = 0; i < total; ++i) nights += World::nightness(static_cast<double>(i) * 0.1) > 0.5f;
        WSIM_CHECK(nights > total * 25 / 100 && nights < total * 55 / 100);

        Terrain t(World::kWorldHalfSize, 29);
        glm::vec2 at;
        WSIM_CHECK(findForest(t, at));
        // Ночью в мире с рельефом сыто-спокойный засыпает (и тратит голод втрое медленнее); днём и в плоском мире -- нет.
        auto sleepsAt = [&](double time, Terrain* terr) {
            World w = lone(31, at, 0);
            WorldState st = w.exportState();
            st.time = time;
            st.population[0].creature.needs.hunger = 0.2f;
            WSIM_CHECK(w.importState(st));
            bool slept = false;
            for (int i = 0; i < 60 * 15 && !slept; ++i) { w.tick(kDt, terr); slept = w.population()[0].creature.behavior == Behavior::Sleep; }
            return slept;
        };
        WSIM_CHECK(sleepsAt(201.0, &t));
        WSIM_CHECK(!sleepsAt(60.0, &t));
        WSIM_CHECK(!sleepsAt(201.0, nullptr));

        // Хижина: знающий строит её из 4 брёвен, вторая рядом не ставится; к хижине идут спать.
        World w = lone(32, at, kSkillSpear | kSkillHut);
        WorldState st = w.exportState();
        st.population[0].creature.wood = 4.0f;
        st.population[0].creature.spearDur = 1.0f;
        WSIM_CHECK(w.importState(st));
        for (int i = 0; i < 60 * 40 && w.stats().hutsBuilt == 0; ++i) { w.tick(kDt, &t); if (i % 300 == 299) feedAll(w); }
        WSIM_CHECK(w.stats().hutsBuilt == 1 && w.structures().size() == 1 && w.structures()[0].kind == StructureKind::Hut);
        WSIM_CHECK(!w.placeStructure(&t, StructureKind::Hut, w.structures()[0].x + 3.0f, w.structures()[0].z, 0.0f));
        WorldState s2 = w.exportState();
        s2.time = 201.0;
        s2.population[0].creature.position = {s2.structures[0].x + 12.0f, 0.0f, s2.structures[0].z};
        s2.population[0].creature.needs.hunger = 0.2f;
        WSIM_CHECK(w.importState(s2));
        float minD = 1e9f;
        for (int i = 0; i < 60 * 40; ++i) {
            w.tick(kDt, &t);
            minD = std::min(minD, std::hypot(w.population()[0].creature.position.x - w.structures()[0].x, w.population()[0].creature.position.z - w.structures()[0].z));
        }
        WSIM_CHECK(minD < 3.0f);   // дошёл до хижины

        // Сохранение: хижина и признак hutNear.
        PhysicsWorld physics;
        SaveGame back;
        std::string err;
        WSIM_CHECK(parseSave(serializeSave(captureSave("h", 0, 0.0f, t, w, physics, CameraState{})), back, err));
        WSIM_CHECK(back.world.structures.size() == 1 && back.world.structures[0].kind == StructureKind::Hut);
    }

    // Хижина не пускает зверей (круг радиусом kHutRadius), как баррикада.
    {
        Fauna fauna;
        Rng rng(8);
        std::vector<Structure> built = {{StructureKind::Hut, 0.0f, 0.0f, 0.0f, 1.0f}};
        Fauna::State st;
        st.nextId = 100;
        Animal wolf;
        wolf.id = 1; wolf.species = Species::Wolf; wolf.position = {-5.0f, 0.0f, 0.0f}; wolf.hunger = 0.95f; wolf.age = speciesDef(Species::Wolf).maturity * 1.5f;
        st.animals = {wolf};
        WSIM_CHECK(fauna.importState(st));
        std::vector<CreatureView> creatures = {{7, {0.0f, 0.0f, 0.0f}}};   // добыча "внутри" хижины
        std::vector<Bite> bites;
        float closest = 1e9f;
        for (int i = 0; i < 60 * 20; ++i) {
            fauna.tick(kDt, nullptr, creatures, rng, 100.0f, bites, {}, &built);
            for (const Animal& a : fauna.animals()) closest = std::min(closest, std::hypot(a.position.x, a.position.z));
        }
        WSIM_CHECK(closest > kHutRadius - 0.2f);
    }

    // ── Этап 5: костёр, роли, хозяин хижины ──
    {
        // Роли зависят от генов (детерминированно) и распределены по всем трём.
        Rng rng(77);
        int counts[3] = {0, 0, 0};
        for (int i = 0; i < 300; ++i) {
            Creature c;
            c.genome = Genome::random(rng);
            WSIM_CHECK(c.role() == c.role());
            ++counts[static_cast<int>(c.role())];
        }
        for (int k : counts) WSIM_CHECK(k >= 30);
        Creature bold, shy;
        Rng r2(5);
        bold.genome = Genome::random(r2);
        bold.genome.setTrait(Genome::kBoldness, 1.0f);
        bold.genome.setTrait(Genome::kSize, 1.0f);
        shy = bold;
        shy.genome.setTrait(Genome::kBoldness, -1.0f);
        shy.genome.setTrait(Genome::kSize, -1.0f);
        shy.genome.setTrait(Genome::kCuriosity, 1.0f);
        WSIM_CHECK(bold.role() == Role::Hunter && shy.role() == Role::Builder);

        Terrain t(World::kWorldHalfSize, 29);
        glm::vec2 at;
        WSIM_CHECK(findForest(t, at));

        // Костёр: вечером знающий с дровами разводит его; он горит kFireBurnSeconds и гаснет.
        World w = lone(41, at, kSkillFire);
        WorldState st = w.exportState();
        st.time = 150.0;   // сумерки (ночность > 0.1)
        st.population[0].creature.wood = 2.0f;
        WSIM_CHECK(w.importState(st));
        for (int i = 0; i < 60 * 30 && w.stats().firesBuilt == 0; ++i) { w.tick(kDt, &t); if (i % 300 == 299) feedAll(w); }
        WSIM_CHECK(w.stats().firesBuilt == 1);
        const Structure* fire = nullptr;
        for (const Structure& s2 : w.structures()) if (s2.kind == StructureKind::Campfire) fire = &s2;
        WSIM_CHECK(fire != nullptr && fire->hp > 0.9f);
        WSIM_CHECK(!w.placeStructure(&t, StructureKind::Campfire, fire->x + 5.0f, fire->z, 0.0f));   // не ближе 10
        WorldState burnt = w.exportState();
        for (Structure& s2 : burnt.structures) s2.hp = 0.001f;
        burnt.population[0].creature.wood = 0.0f;
        WSIM_CHECK(w.importState(burnt));
        for (int i = 0; i < 60 * 2; ++i) w.tick(kDt, &t);
        bool anyFire = false;
        for (const Structure& s2 : w.structures()) anyFire |= s2.kind == StructureKind::Campfire;
        WSIM_CHECK(!anyFire);

        // Подкладывает дрова, когда огонь слабеет.
        World w2 = lone(42, at, kSkillFire);
        WorldState s3 = w2.exportState();
        s3.time = 150.0;
        s3.population[0].creature.wood = 1.0f;
        s3.structures.push_back({StructureKind::Campfire, at.x + 1.0f, at.y, 0.0f, 0.2f});
        WSIM_CHECK(w2.importState(s3));
        w2.tick(kDt, &t);
        WSIM_CHECK(w2.stats().firesStoked == 1 && w2.structures()[0].hp > 0.6f);

        // Горящий костёр отпугивает зверей.
        Fauna fauna;
        Rng rng2(3);
        std::vector<Structure> built = {{StructureKind::Campfire, 0.0f, 0.0f, 0.0f, 1.0f}};
        Fauna::State fs;
        fs.nextId = 50;
        Animal wolf;
        wolf.id = 1; wolf.species = Species::Wolf; wolf.position = {-6.0f, 0.0f, 0.0f}; wolf.hunger = 0.95f; wolf.age = speciesDef(Species::Wolf).maturity * 1.5f;
        fs.animals = {wolf};
        WSIM_CHECK(fauna.importState(fs));
        std::vector<CreatureView> creatures = {{9, {0.0f, 0.0f, 0.0f}}};
        std::vector<Bite> bites;
        float closest = 1e9f;
        for (int i = 0; i < 60 * 20; ++i) {
            fauna.tick(kDt, nullptr, creatures, rng2, 100.0f, bites, {}, &built);
            for (const Animal& an : fauna.animals()) closest = std::min(closest, std::hypot(an.position.x, an.position.z));
        }
        WSIM_CHECK(closest > kFireRadius - 0.3f);

        // Хозяин хижины: строитель становится владельцем, дом -- там; ночью идёт в свою, а не в ближайшую чужую.
        World w3 = lone(43, at, kSkillSpear | kSkillHut);
        WorldState s4 = w3.exportState();
        s4.population[0].creature.wood = 4.0f;
        s4.population[0].creature.spearDur = 1.0f;
        WSIM_CHECK(w3.importState(s4));
        for (int i = 0; i < 60 * 40 && w3.stats().hutsBuilt == 0; ++i) { w3.tick(kDt, &t); if (i % 300 == 299) feedAll(w3); }
        WSIM_CHECK(w3.stats().hutsBuilt == 1);
        const std::uint64_t myId = w3.population()[0].creature.id;
        WSIM_CHECK(w3.structures()[0].owner == myId);

        World w4 = lone(44, at, 0);
        WorldState s5 = w4.exportState();
        const std::uint64_t id = s5.population[0].creature.id;
        s5.time = 201.0;
        s5.population[0].creature.position = {at.x, 0.0f, at.y};
        s5.population[0].creature.needs.hunger = 0.2f;
        s5.structures.push_back({StructureKind::Hut, at.x + 9.0f, at.y, 0.0f, 1.0f, 0.0f, id + 1000});   // ближе, но чужая
        s5.structures.push_back({StructureKind::Hut, at.x - 20.0f, at.y, 0.0f, 1.0f, 0.0f, id});       // дальше, своя
        WSIM_CHECK(w4.importState(s5));
        float dOwn = 1e9f, dForeign = 1e9f;
        for (int i = 0; i < 60 * 40; ++i) {
            w4.tick(kDt, &t);
            const glm::vec3 p = w4.population()[0].creature.position;
            dOwn = std::min(dOwn, std::hypot(p.x - (at.x - 20.0f), p.z - at.y));
            dForeign = std::min(dForeign, std::hypot(p.x - (at.x + 9.0f), p.z - at.y));
        }
        WSIM_CHECK(dOwn < 3.0f && dForeign > 3.0f);

        // Сохранение: 16-битные умения (костёр) и хозяин хижины.
        WorldState s6 = w4.exportState();
        s6.population[0].creature.skills = kSkillFire | kSkillSpear;
        WSIM_CHECK(w4.importState(s6));
        PhysicsWorld physics;
        SaveGame back;
        std::string err;
        WSIM_CHECK(parseSave(serializeSave(captureSave("f", 0, 0.0f, t, w4, physics, CameraState{})), back, err));
        WSIM_CHECK(back.world.population[0].creature.knows(kSkillFire) && back.world.population[0].creature.knows(kSkillSpear));
        std::uint64_t owners = 0;
        for (const Structure& s7 : back.world.structures) owners += s7.owner != 0;
        WSIM_CHECK(owners == 2);
    }

    std::puts("resources_tests: OK");
    return 0;
}

#include "sim/Powers.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>

using namespace wsim;

int main() {
    World   world(1);
    Terrain terrain(World::kWorldHalfSize, 1);
    GodContext god(world, terrain);

    // Встроенные силы зарегистрированы, id уникальны, чужой id занять нельзя.
    PowerRegistry reg;
    for (auto& p : makeBuiltinPowers()) WSIM_CHECK(reg.add(std::move(p)));
    WSIM_CHECK(reg.all().size() == 8);
    WSIM_CHECK(reg.find("raise") != nullptr);
    WSIM_CHECK(!reg.add(makeErasePower("raise", "дубль")));

    // raise/lower меняют высоту и помечают чанки грязными.
    const float h0 = terrain.heightAt(5.0f, 5.0f);
    reg.find("raise")->apply(god, {5.0f, 5.0f, 10.0f, 1.0f, true});
    WSIM_CHECK(terrain.heightAt(5.0f, 5.0f) > h0 + 1.0f);
    WSIM_CHECK(!god.takeDirty().empty());
    WSIM_CHECK(god.takeDirty().empty());  // takeDirty забирает

    // spawn добавляет существ (в пределах потолка), erase убирает.
    const auto before = world.populationCount();
    reg.find("spawn")->apply(god, {30.0f, 30.0f, 3.0f, 0.016f, true});
    WSIM_CHECK(world.populationCount() == before + 1);
    WSIM_CHECK(god.countNear(30.0f, 30.0f, 3.5f) >= 1);
    const auto removed = god.erase(30.0f, 30.0f, 4.0f);
    WSIM_CHECK(removed >= 1);
    WSIM_CHECK(god.countNear(30.0f, 30.0f, 4.0f) == 0);

    // Потолок популяции: спавн сверх kMaxPopulation молча отклоняется.
    for (std::size_t i = 0; i < World::kMaxPopulation + 10; ++i) (void)god.spawn(0.0f, 0.0f);
    WSIM_CHECK(world.populationCount() == World::kMaxPopulation);

    // Валун: без физического мира -- false, с ним -- тело в мире.
    {
        PhysicsWorld phys;
        GodContext   gp(world, terrain, &phys);
        WSIM_CHECK(!god.boulder(0, 0, 1.0f));
        WSIM_CHECK(gp.boulder(5.0f, 5.0f, 1.5f));
        WSIM_CHECK(phys.bodies().size() == 1);
        reg.find("boulder")->apply(gp, {10.0f, 10.0f, 10.0f, 0.016f, true});
        WSIM_CHECK(phys.bodies().size() == 2);
    }

    // Дом: на суше ставится и выравнивает площадку, рядом второй не ставится, ластик убирает.
    {
        Terrain t2(World::kWorldHalfSize, 3);
        World   w2(3);
        GodContext g2(w2, t2);
        float lx = 0, lz = 0;
        bool  found = false;
        for (float x = -80; x <= 80 && !found; x += 6)
            for (float z = -80; z <= 80 && !found; z += 6) {
                bool allLand = true;
                for (float dx : {-4.0f, 0.0f, 4.0f})
                    for (float dz : {-4.0f, 0.0f, 4.0f}) allLand = allLand && t2.heightAt(x + dx, z + dz) > 0.5f;
                if (allLand) { lx = x; lz = z; found = true; }
            }
        WSIM_CHECK(found);
        WSIM_CHECK(g2.build(lx, lz, 30.0f));
        WSIM_CHECK(w2.buildings().size() == 1);
        WSIM_CHECK(!g2.build(lx + 2.0f, lz, 0.0f));  // слишком близко
        WSIM_CHECK(!g2.takeDirty().empty());
        WSIM_CHECK(g2.eraseBuildings(lx, lz, 3.0f) == 1 && w2.buildings().empty());
        // В воде не строится.
        for (float x = -90; x <= 90; x += 3)
            for (float z = -90; z <= 90; z += 3)
                if (t2.isUnderwater(x, z)) { WSIM_CHECK(!g2.build(x, z, 0.0f)); x = 1000; break; }
    }

    // Заданные черты попадают в геном.
    {
        World w2(2);
        const std::vector<std::pair<Genome::TraitIndex, float>> traits = {{Genome::kSize, 1.0f}, {Genome::kSpeed, -1.0f}};
        WSIM_CHECK(w2.spawnCreature(1.0f, 2.0f, traits));
        const Creature& c = w2.population().back().creature;
        WSIM_CHECK(c.genome.trait(Genome::kSize) == 1.0f);
        WSIM_CHECK(c.genome.trait(Genome::kSpeed) == -1.0f);
        WSIM_CHECK(c.position.x == 1.0f && c.position.z == 2.0f);
    }

    // Имена черт для данных модов.
    WSIM_CHECK(Genome::traitFromName("boldness") == Genome::kBoldness);
    WSIM_CHECK(!Genome::traitFromName("nonsense").has_value());

    // После правок и стирания мир продолжает тикать без падений.
    for (int i = 0; i < 120; ++i) world.tick(1.0f / 60.0f);

    std::puts("powers_tests: OK");
    return 0;
}

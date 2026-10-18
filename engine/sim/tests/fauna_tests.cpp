#include "sim/Fauna.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>

using namespace wsim;

namespace {
constexpr float kDt = 1.0f / 60.0f;
constexpr float kHalf = 100.0f;

// Мир-песочница без рельефа: животные и (по желанию) "существа" как точки.
struct Sandbox {
    Fauna                     fauna;
    Rng                       rng{7};
    std::vector<CreatureView> creatures;
    std::vector<Bite>         bites;
    void run(float seconds) {
        for (int i = 0; i < static_cast<int>(seconds / kDt); ++i) fauna.tick(kDt, nullptr, creatures, rng, kHalf, bites);
    }
    // Заменяет животных заданным списком (позиция, вид, голод).
    void setup(std::vector<Animal> animals) {
        Fauna::State st;
        st.nextId = 1000;
        st.animals = std::move(animals);
        for (std::size_t i = 0; i < st.animals.size(); ++i) st.animals[i].id = i + 1;
        WSIM_CHECK(fauna.importState(st));
    }
};

Animal make(Species s, float x, float z, float hunger = 0.2f) {
    Animal a;
    a.species  = s;
    a.position = {x, 0.0f, z};
    a.hunger   = hunger;
    a.age      = speciesDef(s).maturity * 1.2f;
    a.wanderTimer = 1000.0f;   // не менять занятие без повода
    return a;
}
}  // namespace

int main() {
    // ── Таблица видов ──
    {
        std::set<std::string> ids;
        for (int i = 0; i < kSpeciesCount; ++i) {
            const auto s = static_cast<Species>(i);
            const SpeciesDef& d = speciesDef(s);
            WSIM_CHECK(ids.insert(d.id).second && d.name[0] != '\0' && d.size > 0.2f && d.speed > 2.0f && d.cap > 0);
            Species back{};
            WSIM_CHECK(speciesFromId(d.id, back) && back == s);
            float weights = 0;
            for (float w : d.biome) weights += w;
            WSIM_CHECK(weights > 0.0f);                           // у каждого вида есть где жить
            if (d.diet == Diet::Predator) WSIM_CHECK(d.prey != 0);  // у каждого хищника есть добыча
            else WSIM_CHECK(d.prey == 0 && !d.huntsCreatures);
        }
        Species dummy{};
        WSIM_CHECK(!speciesFromId("dragon", dummy));
        // Каждый биом населён, а хищники там реже травоядных; выбор всегда даёт вид, живущий в биоме.
        for (int b = 0; b < kBiomeCount; ++b) {
            int predators = 0, herbivores = 0;
            for (int k = 0; k < 200; ++k) {
                Species s{};
                if (!pickSpeciesFor(static_cast<Biome>(b), static_cast<float>(k) / 200.0f, s)) continue;
                WSIM_CHECK(speciesLivesIn(s, static_cast<Biome>(b)));
                (isPredator(s) ? predators : herbivores)++;
            }
            if (predators + herbivores > 0) WSIM_CHECK(herbivores > 3 * predators);
        }
    }

    // ── Заселение: на суше, внутри мира, не вплотную, воспроизводимо по сиду; хищники только при добыче ──
    {
        int withPredators = 0;
        for (std::uint32_t seed : {5u, 11u, 29u, 77u, 1234u, 9999u}) {
            Terrain t(kHalf, seed);
            Rng r1(seed), r2(seed), r3(seed + 1);
            Fauna a, b, c;
            a.populate(t, r1, kHalf);
            b.populate(t, r2, kHalf);
            c.populate(t, r3, kHalf);
            WSIM_CHECK(a.count() >= 6 && a.count() <= 110);
            WSIM_CHECK(a.count() == b.count());
            std::size_t preyCount = 0, predCount = 0;
            for (std::size_t i = 0; i < a.count(); ++i) {
                const Animal& x = a.animals()[i];
                WSIM_CHECK(x.position.x == b.animals()[i].position.x && x.species == b.animals()[i].species);
                WSIM_CHECK(std::fabs(x.position.x) <= kHalf && std::fabs(x.position.z) <= kHalf);
                WSIM_CHECK(speciesDef(x.species).swims || !t.isUnderwater(x.position.x, x.position.z));
                WSIM_CHECK(std::fabs(x.position.y - t.heightAt(x.position.x, x.position.z)) < 1e-3f);
                (isPredator(x.species) ? predCount : preyCount)++;
                for (std::size_t j = 0; j < i; ++j)
                    WSIM_CHECK(std::hypot(x.position.x - a.animals()[j].position.x, x.position.z - a.animals()[j].position.z) >= Fauna::kMinSpacing - 1e-4f);
            }
            if (predCount > 0) { WSIM_CHECK(preyCount >= 3); ++withPredators; }
            bool differs = a.count() != c.count();
            for (std::size_t i = 0; !differs && i < std::min(a.count(), c.count()); ++i) differs = a.animals()[i].position.x != c.animals()[i].position.x;
            WSIM_CHECK(differs);
            // группы: травоядных больше одиночек (хотя бы в одном из миров найдутся соседи одного вида)
        }
        WSIM_CHECK(withPredators >= 1);   // хоть в одном из миров поселились хищники
    }

    // ── Травоядное убегает от хищника, подошедшего близко, и не бежит от далёкого ──
    {
        Sandbox sb;
        sb.setup({make(Species::Hare, 0.0f, 0.0f), make(Species::Wolf, 10.0f, 0.0f, 0.1f)});  // волк сыт: не охотится
        sb.run(4.0f);
        const Animal& hare = sb.fauna.animals()[0];
        WSIM_CHECK(hare.position.x < -10.0f);                       // убежал от волка (волк справа)
        Sandbox calm;
        calm.setup({make(Species::Hare, 0.0f, 0.0f), make(Species::Wolf, 60.0f, 0.0f, 0.1f)});
        calm.run(4.0f);
        WSIM_CHECK(calm.fauna.animals()[0].state != AnimalState::Flee);  // далеко -- не паникует
        // Существо рядом тоже пугает (но слабее: вплотную).
        Sandbox shy;
        shy.setup({make(Species::Deer, 0.0f, 0.0f)});
        shy.creatures = {{1, {3.0f, 0.0f, 0.0f}}};
        shy.run(2.0f);
        WSIM_CHECK(shy.fauna.animals()[0].position.x < -4.0f);
    }

    // ── Голодный хищник выслеживает и ловит добычу; сытый не охотится ──
    {
        Sandbox sb;
        sb.setup({make(Species::Wolf, 0.0f, 0.0f, 0.85f), make(Species::Hare, 14.0f, 0.0f), make(Species::Hare, 16.0f, 6.0f), make(Species::Deer, 18.0f, -6.0f)});
        float hungerBefore = sb.fauna.animals()[0].hunger;
        bool  hunted = false;
        for (int i = 0; i < static_cast<int>(120.0f / kDt) && sb.fauna.stats().kills == 0; ++i) {
            sb.fauna.tick(kDt, nullptr, sb.creatures, sb.rng, kHalf, sb.bites);
            hunted |= sb.fauna.animals()[0].state == AnimalState::Hunt;
        }
        WSIM_CHECK(hunted && sb.fauna.stats().kills >= 1);          // погнался и поймал
        WSIM_CHECK(sb.fauna.animals()[0].hunger < hungerBefore - 0.3f);  // и наелся
        Sandbox full;
        full.setup({make(Species::Wolf, 0.0f, 0.0f, 0.2f), make(Species::Hare, 10.0f, 0.0f)});
        full.run(30.0f);
        WSIM_CHECK(full.fauna.stats().kills == 0);                  // сытый волк зайца не трогает
    }

    // ── Хищник и существа: голодный без добычи кусает существо, сытый и при добыче -- нет; существо исчезло -- это добыча ──
    {
        Sandbox sb;
        sb.setup({make(Species::Wolf, 0.0f, 0.0f, 0.9f)});
        sb.creatures = {{42, {14.0f, 0.0f, 0.0f}}};
        sb.run(25.0f);
        WSIM_CHECK(sb.bites.size() >= 2);
        for (const Bite& b : sb.bites) WSIM_CHECK(b.creatureId == 42 && b.damage > 0.1f && b.damage < 0.5f);
        // Существа не стало (мир его убил) -- волк засчитывает добычу и наедается.
        const float before = sb.fauna.animals()[0].hunger;
        sb.creatures.clear();
        sb.run(1.0f);
        WSIM_CHECK(sb.fauna.animals()[0].hunger < before - 0.3f && sb.fauna.animals()[0].state == AnimalState::Feed);
        Sandbox peaceful;
        peaceful.setup({make(Species::Fox, 0.0f, 0.0f, 0.95f)});   // лиса на существ не охотится вовсе
        peaceful.creatures = {{7, {6.0f, 0.0f, 0.0f}}};
        peaceful.run(25.0f);
        WSIM_CHECK(peaceful.bites.empty());
        Sandbox hareAround;
        hareAround.setup({make(Species::Wolf, 0.0f, 0.0f, 0.9f), make(Species::Hare, 20.0f, 20.0f)});
        hareAround.creatures = {{9, {8.0f, 0.0f, 0.0f}}};
        hareAround.run(3.0f);
        WSIM_CHECK(hareAround.bites.empty());                      // добыча рядом -- на существо не бросается
    }

    // ── Размножение: сытые взрослые рядом дают потомство; потолок вида держит ──
    {
        Sandbox sb;
        sb.setup({make(Species::Hare, 0.0f, 0.0f, 0.1f), make(Species::Hare, 3.0f, 0.0f, 0.1f)});
        sb.run(300.0f);
        WSIM_CHECK(sb.fauna.stats().births >= 1 && sb.fauna.count(Species::Hare) > 2);
        WSIM_CHECK(static_cast<int>(sb.fauna.count(Species::Hare)) <= speciesDef(Species::Hare).cap);
        // Одиночка не размножается.
        Sandbox lone;
        lone.setup({make(Species::Deer, 0.0f, 0.0f, 0.1f)});
        lone.run(300.0f);
        WSIM_CHECK(lone.fauna.stats().births == 0);
        // Голодные -- тоже нет (травоядные сперва пасутся и быстро сытеют, а у хищников без добычи голод держится).
        Sandbox hungry;
        hungry.setup({make(Species::Wolf, 0.0f, 0.0f, 0.95f), make(Species::Wolf, 3.0f, 0.0f, 0.95f)});
        hungry.run(40.0f);
        WSIM_CHECK(hungry.fauna.stats().births == 0);
        // Потолок: на cap голов рождения прекращаются.
        std::vector<Animal> herd;
        for (int i = 0; i < speciesDef(Species::Hare).cap; ++i) herd.push_back(make(Species::Hare, -80.0f + 6.0f * static_cast<float>(i % 15), -50.0f + 6.0f * static_cast<float>(i / 15), 0.1f));
        Sandbox cap;
        cap.setup(herd);
        cap.run(200.0f);
        WSIM_CHECK(static_cast<int>(cap.fauna.count(Species::Hare)) <= speciesDef(Species::Hare).cap);
    }

    // ── Голодная смерть и старость ──
    {
        Sandbox sb;
        sb.setup({make(Species::Wolf, 0.0f, 0.0f, 0.999f)});   // добычи нет и не будет
        sb.run(120.0f);
        WSIM_CHECK(sb.fauna.count() == 0 && sb.fauna.stats().deathsStarved == 1);
        Sandbox old;
        Animal a = make(Species::Hare, 0.0f, 0.0f, 0.2f);
        a.age = speciesDef(Species::Hare).lifespan - 1.0f;
        old.setup({a});
        old.run(3.0f);
        WSIM_CHECK(old.fauna.count() == 0 && old.fauna.stats().deathsOld == 1);
    }

    // ── Миграция: вид вымер -- с края приходит новая группа; на пустом мире без рельефа ничего не происходит ──
    {
        Terrain t(kHalf, 11);
        Fauna f;
        Rng rng(3);
        std::vector<Bite> bites;
        std::vector<CreatureView> none;
        for (int i = 0; i < static_cast<int>(120.0f / kDt); ++i) f.tick(kDt, &t, none, rng, kHalf, bites);
        WSIM_CHECK(f.count() > 0 && f.stats().immigrants > 0);
        std::size_t preyHeads = 0, predHeads = 0;
        for (const Animal& x : f.animals()) (isPredator(x.species) ? predHeads : preyHeads)++;
        WSIM_CHECK(predHeads == 0 || preyHeads >= 3);   // хищники приходят только вслед за добычей
        Fauna young;
        Rng rng2(3);
        for (int i = 0; i < static_cast<int>(24.0f / kDt); ++i) young.tick(kDt, &t, none, rng2, kHalf, bites);
        WSIM_CHECK(young.count() == 0);                  // до первой проверки (25 с) никто не приходит
        Sandbox flat;
        flat.run(60.0f);
        WSIM_CHECK(flat.fauna.count() == 0);
    }

    // ── Прочее: spawn (потолок, место), поиск, удаление, состояние ──
    {
        Sandbox sb;
        WSIM_CHECK(sb.fauna.spawn(Species::Deer, 10.0f, 10.0f, sb.rng));
        WSIM_CHECK(!sb.fauna.spawn(Species::Deer, 10.3f, 10.0f, sb.rng));    // вплотную нельзя
        WSIM_CHECK(sb.fauna.spawn(Species::Deer, 14.0f, 10.0f, sb.rng, /*adult=*/false));
        WSIM_CHECK(sb.fauna.animals()[1].age == 0.0f && sb.fauna.animals()[0].age > 100.0f);
        std::size_t idx = 99;
        WSIM_CHECK(sb.fauna.nearest(11.0f, 10.5f, 3.0f, idx) && idx == 0);
        WSIM_CHECK(!sb.fauna.nearest(50.0f, 50.0f, 3.0f, idx));
        const Fauna::State st = sb.fauna.exportState();
        Fauna copy;
        WSIM_CHECK(copy.importState(st) && copy.count() == 2 && copy.animals()[0].id == sb.fauna.animals()[0].id);
        Fauna::State bad = st;
        bad.animals[0].position.x = std::nanf("");
        WSIM_CHECK(!copy.importState(bad));
        WSIM_CHECK(sb.fauna.removeInCircle(10.0f, 10.0f, 5.0f) == 2 && sb.fauna.count() == 0);
        // Деревья и вода не пускают: животное у воды не заходит в неё.
        Terrain t(kHalf, 5);
        Fauna f;
        Rng rng(9);
        float wx = 0, wz = 0;
        bool  found = false;
        for (float x = -60; x < 60 && !found; x += 3)
            for (float z = -60; z < 60 && !found; z += 3)
                if (t.heightAt(x, z) > 0.8f && t.heightAt(x, z) < 1.4f && t.isUnderwater(x + 6.0f, z)) { wx = x; wz = z; found = true; }
        WSIM_CHECK(found);
        WSIM_CHECK(f.spawn(Species::Hare, wx, wz, rng));
        std::vector<Bite> bites;
        std::vector<CreatureView> none;
        bool everWet = false;
        for (int i = 0; i < static_cast<int>(40.0f / kDt); ++i) {
            f.tick(kDt, &t, none, rng, kHalf, bites);
            for (const Animal& a : f.animals()) everWet |= t.isUnderwater(a.position.x, a.position.z);
        }
        WSIM_CHECK(!everWet);
    }

    std::puts("fauna_tests: OK");
    return 0;
}

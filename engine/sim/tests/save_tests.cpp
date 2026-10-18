#include "sim/SaveGame.hpp"

#include "sim/ByteIO.hpp"
#include "test_util.hpp"

#include <cstdio>
#include <filesystem>
#include <random>

using namespace wsim;
namespace fs = std::filesystem;

namespace {

constexpr float kDt = 1.0f / 60.0f;

std::vector<std::uint8_t> snapshot(const Terrain& t, const World& w, const PhysicsWorld& p) {
    return serializeSave(captureSave("тест", 1700000000, 12.5f, t, w, p, {}));
}

struct Game {
    Terrain      terrain;
    World        world;
    PhysicsWorld physics;
    explicit Game(std::uint32_t seed) : terrain(World::kWorldHalfSize, seed), world(seed) {}
    void run(int steps) {
        for (int i = 0; i < steps; ++i) {
            world.tick(kDt, &terrain);
            physics.step(kDt, terrain);
        }
    }
};

}  // namespace

int main() {
    // ── Мир с правками рельефа, постройками, валунами и историей ─────────────
    Game a(7);
    a.terrain.raise(10.0f, 10.0f, 12.0f, 6.0f);
    a.terrain.raise(-40.0f, -30.0f, 8.0f, -3.0f);
    a.terrain.flatten(20.0f, -20.0f, 6.0f, 2.0f, 1.0f);
    a.world.settleOnLand(a.terrain);
    a.world.growBerryBushes(a.terrain);  // кусты разных видов: вид тоже должен пережить сохранение
    WSIM_CHECK(a.world.addBuilding(30.0f, 30.0f, 45.0f, 1234));
    WSIM_CHECK(a.world.addBuilding(-30.0f, 20.0f, 120.0f, 99));
    a.physics.addSphere({5.0f, a.terrain.heightAt(5, 5) + 12.0f, 5.0f}, 1.5f);
    a.physics.addSphere({-12.0f, a.terrain.heightAt(-12, 8) + 20.0f, 8.0f}, 2.2f, 1.0f, {1.0f, 0.0f, 0.0f});
    a.run(900);  // 15 секунд: рождаются дети, копится родство, учатся мозги, спит часть валунов

    const auto bytesA = snapshot(a.terrain, a.world, a.physics);

    // ── Круг: сохранить -> разобрать -> восстановить в ЧУЖИЕ объекты -> тот же снимок ──
    SaveGame parsed;
    std::string err;
    WSIM_CHECK(parseSave(bytesA, parsed, err));
    WSIM_CHECK(parsed.meta.name == "тест" && parsed.meta.unixTime == 1700000000 && parsed.meta.seed == 7);
    WSIM_CHECK(parsed.meta.population == a.world.populationCount() && parsed.meta.buildings == 2);

    Game b(12345);  // другой сид: ничего общего, всё должно прийти из файла
    WSIM_CHECK(restoreSave(parsed, b.terrain, b.world, b.physics));
    WSIM_CHECK(snapshot(b.terrain, b.world, b.physics) == bytesA);
    WSIM_CHECK(b.terrain.seed() == 7);
    for (float x : {-50.0f, 10.0f, 33.3f})
        for (float z : {-30.0f, 10.0f, 77.7f}) WSIM_CHECK(a.terrain.heightAt(x, z) == b.terrain.heightAt(x, z));

    // ── Главное: после загрузки мир идёт дальше БИТ В БИТ так же, как оригинал ──
    a.run(600);
    b.run(600);
    WSIM_CHECK(snapshot(a.terrain, a.world, a.physics) == snapshot(b.terrain, b.world, b.physics));
    WSIM_CHECK(a.world.populationCount() > 0);

    // ── Старый формат (v1) читается: новые поля поведения берут значения по умолчанию ──
    {
        const auto v1 = serializeSave(parsed, 1);
        WSIM_CHECK(v1.size() < bytesA.size());
        SaveGame old;
        std::string e1;
        WSIM_CHECK(parseSave(v1, old, e1));
        WSIM_CHECK(old.world.population.size() == parsed.world.population.size());
        for (const auto& in : old.world.population) {
            WSIM_CHECK(in.creature.health == 1.0f && in.creature.matingCooldown == 0.0f && in.creature.behavior == Behavior::Wander);
        }
        Game c(1);
        WSIM_CHECK(restoreSave(old, c.terrain, c.world, c.physics));
        c.run(60);  // и мир живёт дальше
        WSIM_CHECK(c.world.populationCount() > 0);
    }

    // ── Формат v4: яд, недомогания и вкус ягод (по внешнему виду) переживают сохранение; v3 хранит только яд, v2 -- ничего ──
    {
        SaveGame withTaste = parsed;
        WSIM_CHECK(!withTaste.world.population.empty());
        Creature& cr = withTaste.world.population[0].creature;
        cr.toxin = 0.37f;
        cr.diarrhea = 0.5f; cr.nausea = 0.8f; cr.hallucination = 1.25f; cr.vomitStun = 0.9f; cr.visionTimer = 1.5f; cr.visionHeading = 2.0f;
        const auto dark = static_cast<std::size_t>(BerryLook::Dark), orange = static_cast<std::size_t>(BerryLook::Orange);
        cr.taste[dark] = -1.1f;   cr.tasteSamples[dark] = 42;
        cr.taste[orange] = 1.45f; cr.tasteSamples[orange] = 200;
        SaveGame back;
        std::string e3;
        WSIM_CHECK(parseSave(serializeSave(withTaste), back, e3));
        const Creature& rc = back.world.population[0].creature;
        WSIM_CHECK(rc.toxin == 0.37f && rc.taste[dark] == -1.1f && rc.tasteSamples[dark] == 42 &&
                   rc.taste[orange] == 1.45f && rc.tasteSamples[orange] == 200);
        WSIM_CHECK(rc.diarrhea == 0.5f && rc.nausea == 0.8f && rc.hallucination == 1.25f && rc.vomitStun == 0.9f &&
                   rc.visionTimer == 1.5f && rc.visionHeading == 2.0f);
        WSIM_CHECK(rc.knowsBerry(BerryKind::Blackberry) && rc.knowsBerry(BerryKind::Nightshade) && rc.knowsBerry(BerryKind::Henbane));  // группы

        cr.mealLook = static_cast<std::uint8_t>(BerryLook::Dark);
        cr.sinceBite = 7.5f;
        cr.grudgeId = 99; cr.grudge = 0.7f;
        cr.carryKind = static_cast<std::uint8_t>(BerryKind::Henbane);
        cr.carryAmount = 0.12f; cr.carryAge = 3.0f;
        SaveGame back5;
        WSIM_CHECK(parseSave(serializeSave(withTaste), back5, e3));
        const Creature& r5 = back5.world.population[0].creature;
        WSIM_CHECK(r5.mealLook == static_cast<std::uint8_t>(BerryLook::Dark) && r5.sinceBite == 7.5f && r5.grudgeId == 99 && r5.grudge == 0.7f &&
                   r5.carryKind == static_cast<std::uint8_t>(BerryKind::Henbane) && r5.carryAmount == 0.12f && r5.carryAge == 3.0f);
        SaveGame v4back;  // v4: недомогания и вкус есть, зуба и трапезы нет
        WSIM_CHECK(parseSave(serializeSave(withTaste, 4), v4back, e3));
        const Creature& c4 = v4back.world.population[0].creature;
        WSIM_CHECK(c4.hallucination == 1.25f && c4.knowsBerry(BerryKind::Blackberry) && c4.mealLook == 255 && c4.grudgeId == 0 && c4.carryAmount == 0.0f);

        SaveGame v3back;  // v3: яд есть, вкуса и недомоганий нет
        WSIM_CHECK(parseSave(serializeSave(withTaste, 3), v3back, e3));
        const Creature& c3 = v3back.world.population[0].creature;
        WSIM_CHECK(c3.toxin == 0.37f && c3.diarrhea == 0.0f && c3.hallucination == 0.0f && !c3.knowsLook(BerryLook::Dark));
        SaveGame v2back;
        WSIM_CHECK(parseSave(serializeSave(withTaste, 2), v2back, e3));
        const Creature& oc = v2back.world.population[0].creature;
        WSIM_CHECK(oc.toxin == 0.0f && !oc.knowsBerry(BerryKind::Nightshade) && !oc.knowsBerry(BerryKind::Cloudberry));
    }

    // ── Метаданные читаются отдельно, без разбора мира ───────────────────────
    {
        SaveMeta m;
        WSIM_CHECK(parseSaveMeta(bytesA, m, err));
        WSIM_CHECK(m.name == "тест" && m.population == parsed.meta.population);
    }

    // ── Повреждения: ошибка с понятным текстом, без падений ──────────────────
    {
        SaveGame g;
        std::string e;
        WSIM_CHECK(!parseSave({}, g, e) && !e.empty());
        std::vector<std::uint8_t> bad = bytesA;
        bad[0] = 'X';
        WSIM_CHECK(!parseSave(bad, g, e));  // не та магия
        bad = bytesA;
        bad[8] = 99;
        WSIM_CHECK(!parseSave(bad, g, e) && e.find("нов") != std::string::npos);  // версия новее
        bad = bytesA;
        bad[bad.size() / 2] ^= 0xFF;
        WSIM_CHECK(!parseSave(bad, g, e) && e.find("контрольн") != std::string::npos);  // битый байт в нагрузке
        for (std::size_t cut : {std::size_t{0}, std::size_t{7}, std::size_t{12}, std::size_t{40}, bytesA.size() / 3, bytesA.size() - 1}) {
            std::vector<std::uint8_t> shortBuf(bytesA.begin(), bytesA.begin() + static_cast<long>(cut));
            WSIM_CHECK(!parseSave(shortBuf, g, e));  // обрезанный файл
        }
    }

    // ── Подделка с верной контрольной суммой: случайная порча нагрузки не роняет и не ест память ──
    {
        // Смещения: магия 8 + версия 4 + размер сводки 4 + сводка + размер нагрузки 4 + crc 4.
        const std::uint32_t metaSize = static_cast<std::uint32_t>(bytesA[12]) | static_cast<std::uint32_t>(bytesA[13]) << 8;
        const std::size_t   payloadStart = 8 + 4 + 4 + metaSize + 4 + 4;
        std::mt19937 rng(5);
        int rejected = 0, accepted = 0;
        for (int iter = 0; iter < 400; ++iter) {
            std::vector<std::uint8_t> m = bytesA;
            const int flips = 1 + static_cast<int>(rng() % 6);
            for (int f = 0; f < flips; ++f) m[payloadStart + rng() % (m.size() - payloadStart)] = static_cast<std::uint8_t>(rng());
            const std::uint32_t crc = crc32(std::span<const std::uint8_t>(m).subspan(payloadStart));
            for (int i = 0; i < 4; ++i) m[payloadStart - 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(crc >> (8 * i));
            SaveGame g;
            std::string e;
            (parseSave(m, g, e) ? accepted : rejected)++;
        }
        WSIM_CHECK(rejected + accepted == 400);
        WSIM_CHECK(rejected > 0);

        // Счётчик на 4 миллиарда существ с верной суммой: отказ, а не гигабайты памяти.
        ByteWriter w;
        w.u32(0xFFFFFFFFu);
        std::vector<std::uint8_t> body;
        auto putSection = [&](std::uint32_t t, const std::vector<std::uint8_t>& d) {
            ByteWriter s; s.u32(t); s.u32(static_cast<std::uint32_t>(d.size())); s.raw(d);
            body.insert(body.end(), s.bytes().begin(), s.bytes().end());
        };
        ByteWriter terr; terr.u32(1); terr.f32(100.0f); terr.u32(0);
        putSection(0x52524554u, terr.bytes());  // "TERR"
        ByteWriter world; world.str("x"); world.u64(1); world.u32(0xFFFFFFFFu);
        putSection(0x444C5257u, world.bytes());  // "WRLD"
        ByteWriter meta; meta.str("x"); meta.i64(0); meta.u32(0); meta.f32(0); meta.u32(0); meta.u32(0); meta.u32(0); meta.u32(0);
        ByteWriter file;
        for (char c : {'W', 'S', 'I', 'M', 'S', 'A', 'V', 'E'}) file.u8(static_cast<std::uint8_t>(c));
        file.u32(kSaveVersion); file.u32(static_cast<std::uint32_t>(meta.bytes().size())); file.raw(meta.bytes());
        file.u32(static_cast<std::uint32_t>(body.size())); file.u32(crc32(body)); file.raw(body);
        SaveGame g;
        std::string e;
        WSIM_CHECK(!parseSave(file.bytes(), g, e));
    }

    // ── Хранилище: атомарная запись, список (новые сначала), удаление, защита имён ──
    {
        const fs::path dir = fs::temp_directory_path() / "wsim_save_test";
        fs::remove_all(dir);
        SaveStore store(dir);
        std::string e;
        SaveGame older = parsed, newer = parsed;
        older.meta.unixTime = 1000; older.meta.name = "старое";
        newer.meta.unixTime = 2000; newer.meta.name = "новое";
        WSIM_CHECK(store.save("world_a", older, e));
        WSIM_CHECK(store.save("world_b", newer, e));
        WSIM_CHECK(store.save("world_a", older, e));  // перезапись того же слота
        for (const auto& entry : fs::directory_iterator(dir)) WSIM_CHECK(entry.path().extension() == ".wsim");  // временных файлов не осталось

        auto list = store.list();
        WSIM_CHECK(list.size() == 2 && list[0].slot == "world_b" && list[1].slot == "world_a");
        WSIM_CHECK(list[0].valid && list[0].meta.name == "новое");

        SaveGame loaded;
        WSIM_CHECK(store.load("world_b", loaded, e) && loaded.meta.name == "новое");
        WSIM_CHECK(!store.load("missing", loaded, e));

        // Битый файл виден в списке (чтобы его можно было удалить), но не грузится.
        { std::FILE* f = std::fopen((dir / "broken.wsim").string().c_str(), "wb"); std::fputs("мусор", f); std::fclose(f); }
        list = store.list();
        WSIM_CHECK(list.size() == 3);
        int invalid = 0;
        for (const auto& en : list) if (!en.valid) { ++invalid; WSIM_CHECK(!en.error.empty()); }
        WSIM_CHECK(invalid == 1);

        // Имена слотов: только безопасные символы, никаких путей.
        for (const char* bad : {"", "../evil", "a/b", "a b", "a.b", "x\\y"}) {
            WSIM_CHECK(!SaveStore::validSlot(bad));
            WSIM_CHECK(!store.save(bad, parsed, e));
        }
        WSIM_CHECK(SaveStore::validSlot("world_2024-10-18_21-30"));

        WSIM_CHECK(store.remove("world_a") && !store.exists("world_a"));
        WSIM_CHECK(!store.remove("world_a"));
        fs::remove_all(dir);
    }

    // Виды ягод переживают сохранение; старый файл (без секции BRRY) читается, все кусты -- малина.
    {
        Game g(21);
        g.world.growBerryBushes(g.terrain);
        bool mixed = false;
        for (const auto& f : g.world.food()) mixed |= f.kind != g.world.food().front().kind;
        WSIM_CHECK(g.world.foodCount() >= 18 && mixed);
        const auto bytes = snapshot(g.terrain, g.world, g.physics);
        SaveGame parsed;
        std::string err;
        WSIM_CHECK(parseSave(bytes, parsed, err));
        WSIM_CHECK(parsed.world.food.size() == g.world.foodCount());
        for (std::size_t i = 0; i < parsed.world.food.size(); ++i) WSIM_CHECK(parsed.world.food[i].kind == g.world.food()[i].kind);
    }

    std::puts("save_tests: OK");
    return 0;
}

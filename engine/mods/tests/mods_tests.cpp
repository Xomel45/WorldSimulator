#include "mods/ModManager.hpp"

#include "test_util.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>

using namespace wsim;
namespace fs = std::filesystem;

namespace {
void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}
const ModInfo* findMod(const ModManager& m, const std::string& id) {
    for (const auto& mod : m.mods())
        if (mod.id == id) return &mod;
    return nullptr;
}
}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "wsim_mods_test";
    fs::remove_all(root);

    // Хороший мод: данные + скрипты (функция и таблица с apply).
    write(root / "mods" / "good" / "mod.json", R"({
      "id": "good", "name": "Хороший", "version": "1.0", "api_version": 1,
      "powers": [
        {"id": "hill", "name": "Холм", "type": "raise", "rate": 12, "radius": 9},
        {"id": "wolf", "name": "Волк", "type": "spawn", "traits": {"size": 1, "boldness": 1}, "interval": 0.3},
        {"id": "bolt", "name": "Молния", "type": "script", "script": "scripts/bolt.lua", "interval": -1},
        {"id": "tbl",  "name": "Таблица", "type": "script", "script": "scripts/tbl.lua"},
        {"id": "blue", "name": "Черника", "type": "bush", "berry": "blueberry", "radius": 3},
        {"id": "wild", "name": "Дикие кусты", "type": "bush"},
        {"id": "badberry", "name": "Не то", "type": "bush", "berry": "durian"}
      ]})");
    write(root / "mods" / "good" / "scripts" / "bolt.lua", R"(
      return function(ctx, p)
        ctx:erase(p.x, p.z, p.radius)
        ctx:raise(p.x, p.z, p.radius, -2)
      end)");
    write(root / "mods" / "good" / "scripts" / "tbl.lua", R"(
      local M = {}
      function M.apply(ctx, p)
        local h = ctx:height(p.x, p.z)
        assert(type(h) == "number")
        assert(ctx:spawn(p.x, p.z, {speed = 1, size = -1}))
        assert(type(ctx:bush(p.x + 40, p.z + 40, "cloudberry")) == "boolean")
        assert(type(ctx:bush(p.x - 40, p.z - 40)) == "boolean")
        assert(type(ctx:erase_bushes(p.x + 40, p.z + 40, 1)) == "number")
        assert(ctx:count(p.x, p.z, 5) >= 1)
      end
      return M)");

    // Звук из скрипта: ctx:sound идёт в SoundSink; неизвестное имя -- false; без sink -- false.
    write(root / "mods" / "snd" / "mod.json", R"({"id":"snd","name":"Звук","api_version":1,
      "powers":[{"id":"beep","type":"script","script":"beep.lua"}]})");
    write(root / "mods" / "snd" / "beep.lua", R"(
      return function(ctx, p)
        assert(ctx:sound("thud", p.x, p.z, 0.5) == true)
        assert(ctx:sound("not_a_sound", p.x, p.z) == false)
      end)");

    // Злые и сломанные моды.
    write(root / "mods" / "evil" / "mod.json", R"({
      "id": "evil", "name": "Злой", "api_version": 1,
      "powers": [
        {"id": "loop",   "type": "script", "script": "loop.lua"},
        {"id": "os",     "type": "script", "script": "os.lua"},
        {"id": "io",     "type": "script", "script": "io.lua"},
        {"id": "pcallloop", "type": "script", "script": "pcallloop.lua"},
        {"id": "crash",  "type": "script", "script": "crash.lua"},
        {"id": "escape", "type": "script", "script": "../good/scripts/bolt.lua"},
        {"id": "syntax", "type": "script", "script": "syntax.lua"},
        {"id": "load",   "type": "script", "script": "load.lua"},
        {"id": "ok",     "type": "erase"}
      ]})");
    write(root / "mods" / "evil" / "loop.lua",  "return function(ctx, p) while true do end end");
    write(root / "mods" / "evil" / "pcallloop.lua", "return function(ctx, p) while true do pcall(function() while true do end end) end end");
    write(root / "mods" / "evil" / "os.lua",    "return function(ctx, p) os.execute('touch /tmp/wsim_pwned') end");
    write(root / "mods" / "evil" / "io.lua",    "return function(ctx, p) io.open('/etc/passwd') end");
    write(root / "mods" / "evil" / "crash.lua", "return function(ctx, p) local x = nil; x.y = 1 end");
    write(root / "mods" / "evil" / "syntax.lua", "return function(");
    write(root / "mods" / "evil" / "load.lua",  "return function(ctx, p) load('return 1')() end");

    write(root / "mods" / "old" / "mod.json", R"({"id": "old", "name": "Старый", "api_version": 99})");
    write(root / "mods" / "broken" / "mod.json", "{ not json");
    write(root / "mods" / "notamod" / "readme.txt", "no manifest -> ignored");

    ModManager mgr({root / "mods"}, root / "cfg" / "disabled.cfg");
    PowerRegistry reg;
    for (auto& p : makeBuiltinPowers()) reg.add(std::move(p));
    mgr.reload(reg);

    // Находятся 4 мода (notamod без mod.json игнорируется), совместимые валидны.
    WSIM_CHECK(mgr.mods().size() == 5);
    WSIM_CHECK(findMod(mgr, "good") && findMod(mgr, "good")->valid && findMod(mgr, "good")->errors->size() == 1  /* неизвестный вид ягод у badberry */);
    WSIM_CHECK(findMod(mgr, "old") && !findMod(mgr, "old")->valid && !findMod(mgr, "old")->errors->empty());
    WSIM_CHECK(findMod(mgr, "broken") && !findMod(mgr, "broken")->valid);

    // Силы модов регистрируются с префиксом, встроенные остаются.
    WSIM_CHECK(reg.find("good.hill") && reg.find("good.wolf") && reg.find("good.bolt") && reg.find("good.tbl"));
    WSIM_CHECK(reg.find("raise"));
    WSIM_CHECK(reg.find("good.hill")->source == "good" && reg.find("good.hill")->radius == 9.0f);
    WSIM_CHECK(reg.find("good.bolt")->interval < 0.0f);

    // Побег из папки, синтаксис -- отсеяны при загрузке; безопасные силы злого мода остались.
    const ModInfo* evil = findMod(mgr, "evil");
    WSIM_CHECK(evil && evil->valid);
    WSIM_CHECK(!reg.find("evil.escape") && !reg.find("evil.syntax"));
    WSIM_CHECK(reg.find("evil.ok"));
    WSIM_CHECK(evil->errors->size() >= 2);

    World world(5);
    Terrain terrain(World::kWorldHalfSize, 5);
    GodContext god(world, terrain);

    // Звук из скрипта.
    {
        struct Sink : wsim::SoundSink {
            int played = 0;
            bool playNamed(std::string_view name, const glm::vec3&, float) override {
                if (name != "thud") return false;
                ++played;
                return true;
            }
        } sink;
        GodContext withSink(world, terrain, nullptr, &sink);
        const PowerDef* beep = reg.find("snd.beep");
        WSIM_CHECK(beep != nullptr);
        beep->apply(withSink, {3.0f, 4.0f, 2.0f, 0.016f, true});
        WSIM_CHECK(sink.played == 1);
        WSIM_CHECK(findMod(mgr, "snd")->errors->empty());
    }

    // Кусты: JSON-сила с видом, без вида (по биому) и с неизвестным видом (ошибка, силы нет).
    WSIM_CHECK(reg.find("good.blue") && reg.find("good.wild") && !reg.find("good.badberry"));
    WSIM_CHECK(reg.find("good.blue")->description.find("Черника") != std::string::npos);
    {
        const std::size_t before = world.foodCount();
        bool planted = false;
        for (float x = -60; x <= 60 && !planted; x += 7)
            for (float z = -60; z <= 60 && !planted; z += 7)
                if (!terrain.isUnderwater(x, z)) {
                    reg.find("good.blue")->apply(god, {x, z, 0.5f, 0.016f, true});  // радиус 0.5 -- почти точка
                    planted = world.foodCount() > before;
                }
        WSIM_CHECK(planted && world.food().back().kind == BerryKind::Blueberry);
    }

    // Рабочие скрипты действуют на мир.
    const float h0 = terrain.heightAt(10.0f, 10.0f);
    reg.find("good.hill")->apply(god, {10.0f, 10.0f, 9.0f, 1.0f / 60.0f, true});
    WSIM_CHECK(terrain.heightAt(10.0f, 10.0f) > h0);
    const auto pop0 = world.populationCount();
    reg.find("good.tbl")->apply(god, {20.0f, 20.0f, 4.0f, 0.016f, true});
    WSIM_CHECK(world.populationCount() == pop0 + 1);
    reg.find("good.wolf")->apply(god, {25.0f, 25.0f, 3.0f, 0.016f, true});
    WSIM_CHECK(world.populationCount() == pop0 + 2);
    reg.find("good.bolt")->apply(god, {20.0f, 20.0f, 4.0f, 0.016f, true});
    WSIM_CHECK(world.countCreatures(20.0f, 20.0f, 3.0f) == 0);
    // Единственная ошибка мода -- неизвестный вид ягод у "badberry".
    WSIM_CHECK(findMod(mgr, "good")->errors->size() == 1 && findMod(mgr, "good")->errors->front().find("ягод") != std::string::npos);

    // Песочница: os/io/load недоступны; бесконечный цикл обрывается по бюджету
    // инструкций (а не вешает тест); краш скрипта отключает силу, а не игру.
    // Эти силы могли не пройти загрузку (скрипт исполняется при загрузке только до return),
    // поэтому проверяем те, что дошли до реестра.
    const auto t0 = std::chrono::steady_clock::now();
    for (const char* id : {"evil.loop", "evil.pcallloop", "evil.os", "evil.io", "evil.crash", "evil.load"}) {
        const PowerDef* p = reg.find(id);
        WSIM_CHECK(p != nullptr);
        p->apply(god, {0.0f, 0.0f, 3.0f, 0.016f, true});
        p->apply(god, {0.0f, 0.0f, 3.0f, 0.016f, false});  // второй вызов -- сила уже отключена, ничего не делает
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    WSIM_CHECK(ms < 5000);
    WSIM_CHECK(evil->errors->size() >= 2 + 5);          // по ошибке на каждую сломанную силу
    WSIM_CHECK(!fs::exists("/tmp/wsim_pwned"));          // os.execute не выполнился
    WSIM_CHECK(reg.find("evil.ok"));

    // Выключение: силы исчезают, выбор сохраняется на диск и переживает новый менеджер.
    mgr.setEnabled(reg, "good", false);
    WSIM_CHECK(!reg.find("good.hill") && !findMod(mgr, "good")->enabled);
    WSIM_CHECK(reg.find("raise"));
    {
        ModManager mgr2({root / "mods"}, root / "cfg" / "disabled.cfg");
        PowerRegistry reg2;
        mgr2.reload(reg2);
        WSIM_CHECK(!reg2.find("good.hill"));
        WSIM_CHECK(reg2.find("evil.ok"));
        mgr2.setEnabled(reg2, "good", true);
        WSIM_CHECK(reg2.find("good.hill"));
    }

    fs::remove_all(root);
    std::puts("mods_tests: OK");
    return 0;
}

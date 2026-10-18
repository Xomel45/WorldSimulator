#include "mods/ModManager.hpp"

#include "core/Logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <regex>
#include <unordered_set>

namespace wsim {

namespace fs   = std::filesystem;
using json     = nlohmann::json;

namespace {

bool validId(const std::string& s) { return std::regex_match(s, std::regex("[a-z0-9_]{1,32}")); }

std::string str(const json& j, const char* key, const std::string& def = {}) {
    if (!j.contains(key) || !j[key].is_string()) return def;
    return j[key].get<std::string>();
}

float number(const json& j, const char* key, float def) {
    if (!j.contains(key) || !j[key].is_number()) return def;
    return j[key].get<float>();
}

// Путь внутри папки мода: без выхода наружу через "..", без абсолютных путей.
std::optional<fs::path> insideMod(const fs::path& modDir, const std::string& rel) {
    const fs::path p = fs::path(rel).lexically_normal();
    if (p.empty() || p.is_absolute() || *p.begin() == "..") return std::nullopt;
    return modDir / p;
}

}  // namespace

ModManager::ModManager(std::vector<fs::path> roots, fs::path disabledFile)
    : m_roots(std::move(roots)), m_disabledFile(std::move(disabledFile)) {}

void ModManager::loadDisabled() {
    m_disabledIds.clear();
    std::ifstream in(m_disabledFile);
    std::string   line;
    while (std::getline(in, line))
        if (!line.empty()) m_disabledIds.push_back(line);
}

void ModManager::saveDisabled() const {
    std::error_code ec;
    fs::create_directories(m_disabledFile.parent_path(), ec);
    std::ofstream out(m_disabledFile, std::ios::trunc);
    for (const auto& id : m_disabledIds) out << id << "\n";
}

void ModManager::discover() {
    m_mods.clear();
    std::unordered_set<std::string> seen;
    for (const auto& root : m_roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        std::vector<fs::path> dirs;
        for (const auto& entry : fs::directory_iterator(root, ec))
            if (entry.is_directory()) dirs.push_back(entry.path());
        std::sort(dirs.begin(), dirs.end());

        for (const auto& dir : dirs) {
            const fs::path manifest = dir / "mod.json";
            if (!fs::exists(manifest, ec)) continue;  // не мод -- молча пропускаем

            ModInfo mod;
            mod.dir  = dir;
            mod.id   = dir.filename().string();
            mod.name = mod.id;
            try {
                std::ifstream in(manifest);
                const json j = json::parse(in);
                mod.id          = str(j, "id", mod.id);
                mod.name        = str(j, "name", mod.id);
                mod.version     = str(j, "version", "0");
                mod.author      = str(j, "author");
                mod.description = str(j, "description");
                const int api   = j.contains("api_version") && j["api_version"].is_number_integer() ? j["api_version"].get<int>() : 0;
                if (!validId(mod.id))
                    mod.errors->push_back("id мода должен состоять из a-z, 0-9 и _ (до 32 символов)");
                else if (api != kModApiVersion)
                    mod.errors->push_back("мод для API " + std::to_string(api) + ", игра поддерживает API " + std::to_string(kModApiVersion));
                else
                    mod.valid = true;
            } catch (const std::exception& e) {
                mod.errors->push_back(std::string("mod.json: ") + e.what());
            }
            if (mod.valid && !seen.insert(mod.id).second) {
                mod.valid = false;
                mod.errors->push_back("мод с таким id уже загружен из другой папки");
            }
            mod.enabled = mod.valid && std::find(m_disabledIds.begin(), m_disabledIds.end(), mod.id) == m_disabledIds.end();
            m_mods.push_back(std::move(mod));
        }
    }
}

void ModManager::loadMod(ModInfo& mod, PowerRegistry& registry) {
    registry.removeFromSource(mod.id);
    mod.sandbox.reset();
    mod.powerCount = 0;
    if (!mod.valid || !mod.enabled) return;
    mod.errors->clear();

    json j;
    try {
        std::ifstream in(mod.dir / "mod.json");
        j = json::parse(in);
    } catch (const std::exception& e) {
        mod.errors->push_back(e.what());
        return;
    }
    if (!j.contains("powers") || !j["powers"].is_array()) return;  // мод без сил -- допустим

    for (const json& pj : j["powers"]) {
        const std::string pid = str(pj, "id");
        if (!validId(pid)) { mod.errors->push_back("сила с недопустимым id: \"" + pid + "\""); continue; }
        const std::string type = str(pj, "type", "script");
        const std::string name = str(pj, "name", pid);
        const std::string full = mod.id + "." + pid;

        PowerDef def;
        if (type == "raise" || type == "lower") {
            const float rate = std::abs(number(pj, "rate", 8.0f));
            def = makeRaisePower(full, name, type == "lower" ? -rate : rate);
        } else if (type == "flatten") {
            def = makeFlattenPower(full, name, number(pj, "rate", 6.0f));
        } else if (type == "erase") {
            def = makeErasePower(full, name);
        } else if (type == "spawn") {
            std::vector<std::pair<Genome::TraitIndex, float>> traits;
            if (pj.contains("traits") && pj["traits"].is_object())
                for (const auto& [k, v] : pj["traits"].items()) {
                    const auto idx = Genome::traitFromName(k);
                    if (!idx) { mod.errors->push_back(pid + ": неизвестная черта \"" + k + "\""); continue; }
                    if (v.is_number()) traits.emplace_back(*idx, std::clamp(v.get<float>(), -1.0f, 1.0f));
                }
            def = makeSpawnPower(full, name, std::move(traits), number(pj, "interval", 0.15f));
        } else if (type == "script") {
            const auto path = insideMod(mod.dir, str(pj, "script"));
            if (!path) { mod.errors->push_back(pid + ": поле script должно быть путём внутри папки мода"); continue; }
            if (!mod.sandbox) mod.sandbox = std::make_shared<LuaSandbox>(mod.id);
            const auto loaded = mod.sandbox->loadPowerScript(*path);
            if (loaded.handler < 0) { mod.errors->push_back(pid + ": " + loaded.error); continue; }
            def.id = full;
            def.name = name;
            // Ошибка времени исполнения отключает только эту силу и попадает в список ошибок мода.
            auto sandbox = mod.sandbox;
            auto errors  = mod.errors;
            auto broken  = std::make_shared<bool>(false);
            def.apply = [sandbox, errors, broken, handler = loaded.handler, pid](GodContext& g, const PowerCall& c) {
                if (*broken) return;
                if (const std::string err = sandbox->call(handler, g, c); !err.empty()) {
                    *broken = true;
                    errors->push_back(pid + ": " + err);
                    Logger::error("[mod] сила {} отключена: {}", pid, err);
                }
            };
        } else {
            mod.errors->push_back(pid + ": неизвестный тип силы \"" + type + "\"");
            continue;
        }

        def.name        = name;
        def.description = str(pj, "description", def.description);
        def.source      = mod.id;
        def.radius      = number(pj, "radius", def.radius);
        def.minRadius   = number(pj, "min_radius", def.minRadius);
        def.maxRadius   = std::min(number(pj, "max_radius", def.maxRadius), 100.0f);
        def.interval    = number(pj, "interval", def.interval);
        if (registry.add(std::move(def))) ++mod.powerCount;
        else mod.errors->push_back(pid + ": id силы уже занят");
    }
    if (!mod.errors->empty()) Logger::warn("[mod {}] {} проблем(ы) при загрузке", mod.id, mod.errors->size());
    else Logger::info("[mod {}] загружен: {} сил", mod.id, mod.powerCount);
}

void ModManager::reload(PowerRegistry& registry) {
    for (const auto& m : m_mods) registry.removeFromSource(m.id);
    loadDisabled();
    discover();
    for (auto& m : m_mods) loadMod(m, registry);
}

void ModManager::setEnabled(PowerRegistry& registry, const std::string& modId, bool enabled) {
    for (auto& m : m_mods) {
        if (m.id != modId || !m.valid) continue;
        m.enabled = enabled;
        std::erase(m_disabledIds, modId);
        if (!enabled) m_disabledIds.push_back(modId);
        saveDisabled();
        loadMod(m, registry);
    }
}

}  // namespace wsim

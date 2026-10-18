#pragma once

#include "mods/LuaSandbox.hpp"
#include "sim/Powers.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace wsim {

// Версия API модов: растёт при несовместимых изменениях контракта (mod.json, ctx).
// Мод с другим api_version не загружается и объясняет почему на странице "Моды".
inline constexpr int kModApiVersion = 1;

struct ModInfo {
    std::string           id, name, version, author, description;
    std::filesystem::path dir;
    bool                  valid   = false;   // mod.json разобран и совместим
    bool                  enabled = true;
    int                   powerCount = 0;
    // Ошибки манифеста и скриптов; делится с обработчиками сил, которые дописывают ошибки времени исполнения.
    std::shared_ptr<std::vector<std::string>> errors = std::make_shared<std::vector<std::string>>();
    std::shared_ptr<LuaSandbox>               sandbox;  // живёт, пока мод включён и есть скрипты
};

// Находит моды в папках, читает mod.json, собирает их силы в PowerRegistry.
//
//   mods/<папка>/mod.json       -- манифест (id, name, version, author, description, api_version, powers[])
//   mods/<папка>/scripts/*.lua  -- скрипты сил типа "script"
//
// Силы получают id "<mod id>.<power id>" и source = id мода: мод не может
// подменить встроенную или чужую силу. Выключенные моды (список в файле
// конфигурации) не загружаются.
class ModManager {
public:
    // Папки поиска по порядку; при совпадении id побеждает первая. disabledFile -- где хранить выключенные моды.
    ModManager(std::vector<std::filesystem::path> roots, std::filesystem::path disabledFile);

    // Перечитать диск и пересобрать силы модов в registry (силы прежних модов убираются).
    void reload(PowerRegistry& registry);

    // Включить/выключить мод и сразу пересобрать его силы.
    void setEnabled(PowerRegistry& registry, const std::string& modId, bool enabled);

    [[nodiscard]] std::span<const ModInfo> mods() const noexcept { return m_mods; }
    [[nodiscard]] const std::vector<std::filesystem::path>& roots() const noexcept { return m_roots; }

private:
    void discover();
    void loadDisabled();
    void saveDisabled() const;
    void loadMod(ModInfo& mod, PowerRegistry& registry);

    std::vector<std::filesystem::path> m_roots;
    std::filesystem::path              m_disabledFile;
    std::vector<std::string>           m_disabledIds;
    std::vector<ModInfo>               m_mods;
};

}  // namespace wsim

#pragma once

#include "sim/Physics.hpp"
#include "sim/Terrain.hpp"
#include "sim/World.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wsim {

// 1 -- первая версия; 2 -- состояние поведения существ (здоровье, перезарядка, занятие, память еды).
// Файлы версии 1 читаются: новые поля берут значения по умолчанию.
inline constexpr std::uint32_t kSaveVersion = 3;  // 3: яд и вкус ягод у существ

// Краткая сводка для списка сохранений: читается из заголовка без разбора всего мира.
struct SaveMeta {
    std::string   name;
    std::int64_t  unixTime = 0;      // когда сохранено
    std::uint32_t seed = 0;          // сид рельефа
    float         playSeconds = 0;   // сколько игры прошло в этом мире
    std::uint32_t population = 0, buildings = 0, boulders = 0, editedTiles = 0;
};

struct CameraState {
    glm::vec3 target{0.0f};
    float     yaw = -90.0f, pitch = -45.0f, distance = 25.0f;
};

// Всё, что составляет игру: рельеф (сид + правки), мир (существа, еда, постройки), валуны, камера.
// Настройки, выбор силы и моды в сохранение не входят -- это не состояние мира.
struct SaveGame {
    SaveMeta                    meta;
    std::uint32_t               terrainSeed = 0;
    float                       terrainHalfSize = 100.0f;
    std::vector<Terrain::EditTileData> terrainEdits;
    WorldState                  world;
    std::vector<RigidBody>      bodies;
    CameraState                 camera;
};

// Снимок и применение (без файлов): чистые функции над sim-объектами.
[[nodiscard]] SaveGame captureSave(const std::string& name, std::int64_t unixTime, float playSeconds, const Terrain& terrain,
                                   const World& world, const PhysicsWorld& physics, const CameraState& camera);

// version < kSaveVersion пишет старый формат (нужно тестам совместимости).
[[nodiscard]] std::vector<std::uint8_t> serializeSave(const SaveGame& save, std::uint32_t version = kSaveVersion);

// Разбор: false и текст ошибки (по-русски, для игрока), если файл битый, обрезан или от более новой версии.
[[nodiscard]] bool parseSave(std::span<const std::uint8_t> bytes, SaveGame& out, std::string& error);
[[nodiscard]] bool parseSaveMeta(std::span<const std::uint8_t> bytes, SaveMeta& out, std::string& error);  // только заголовок

// Применить загруженное к живым объектам. false -- состояние некорректно (тогда ничего из них не гарантировано:
// вызывающий должен собирать в новые объекты и подменять только при успехе).
[[nodiscard]] bool restoreSave(const SaveGame& save, Terrain& terrain, World& world, PhysicsWorld& physics);

// ─── Хранилище на диске ──────────────────────────────────────────────────────
struct SaveEntry {
    std::filesystem::path path;
    std::string           slot;      // имя файла без расширения
    SaveMeta              meta;
    bool                  valid = false;
    std::string           error;     // почему не читается (для списка)
};

// Папка сохранений: запись атомарная (во временный файл, затем переименование), чтобы сбой посреди
// записи не оставил половину мира.
class SaveStore {
public:
    explicit SaveStore(std::filesystem::path dir) : m_dir(std::move(dir)) {}

    bool save(const std::string& slot, const SaveGame& game, std::string& error) const;
    bool load(const std::string& slot, SaveGame& out, std::string& error) const;
    bool remove(const std::string& slot) const;
    [[nodiscard]] bool exists(const std::string& slot) const;
    // Новые сначала; битые файлы тоже в списке (valid=false), чтобы их можно было удалить.
    [[nodiscard]] std::vector<SaveEntry> list() const;
    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return m_dir; }
    [[nodiscard]] std::filesystem::path pathFor(const std::string& slot) const;

    // Имя слота только из безопасных символов (без путей).
    [[nodiscard]] static bool validSlot(const std::string& slot);

private:
    std::filesystem::path m_dir;
};

}  // namespace wsim

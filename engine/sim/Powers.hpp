#pragma once

#include "sim/Genome.hpp"
#include "sim/Physics.hpp"
#include "sim/SoundSink.hpp"
#include "sim/Terrain.hpp"
#include "sim/World.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

namespace wsim {

// Всё, чем сила бога может вмешаться в мир. Единственный путь для встроенных
// сил и для скриптов модов (Lua получает именно этот объект), так что мод
// умеет ровно то же, что и сама игра, и не больше. Собирает прямоугольники
// изменённого рельефа -- рендер по ним перестраивает затронутые чанки.
class GodContext {
public:
    GodContext(World& world, Terrain& terrain, PhysicsWorld* physics = nullptr, SoundSink* sound = nullptr)
        : m_world(world), m_terrain(terrain), m_physics(physics), m_sound(sound) {}

    [[nodiscard]] float height(float x, float z) const noexcept { return m_terrain.heightAt(x, z); }
    void raise(float x, float z, float radius, float amount) { m_dirty.push_back(m_terrain.raise(x, z, radius, amount)); }
    void flatten(float x, float z, float radius, float target, float rate) {
        m_dirty.push_back(m_terrain.flatten(x, z, radius, target, rate));
    }
    bool spawn(float x, float z, std::span<const std::pair<Genome::TraitIndex, float>> traits = {}) {
        return m_world.spawnCreature(x, z, traits);
    }
    std::size_t erase(float x, float z, float radius) { return m_world.removeCreatures(x, z, radius); }
    // Звук в точке (x, z) на поверхности; false -- звука нет (нет слоя звука или неизвестное имя).
    bool sound(std::string_view name, float x, float z, float volume = 1.0f) {
        return m_sound && m_sound->playNamed(name, {x, m_terrain.heightAt(x, z), z}, volume);
    }
    // Дом в (x, z): на суше (не в воде), не ближе kBuildingSpacing к другому; под ним выравнивается площадка.
    bool build(float x, float z, float yaw) {
        constexpr float kHalf = 2.8f;  // чуть больше половины самого большого дома
        for (float dx : {-kHalf, kHalf})
            for (float dz : {-kHalf, kHalf})
                if (m_terrain.isUnderwater(x + dx, z + dz)) return false;
        if (m_terrain.isUnderwater(x, z)) return false;
        const auto seed = static_cast<std::uint32_t>(std::fabs(x * 7.31f + z * 13.7f) * 31.0f);
        if (!m_world.addBuilding(x, z, yaw, seed)) return false;
        const float base = m_terrain.heightAt(x, z);
        for (int i = 0; i < 3; ++i) flatten(x, z, kHalf + 1.6f, base, 0.8f);
        return true;
    }
    std::size_t eraseBuildings(float x, float z, float radius) { return m_world.removeBuildings(x, z, radius); }
    // Ягодный куст: kind -- этого вида, иначе подходящий биому точки. На суше, не ближе kBushSpacing к другому.
    bool bush(float x, float z, std::optional<BerryKind> kind = std::nullopt) {
        if (m_terrain.isUnderwater(x, z)) return false;
        return kind ? m_world.plantBush(x, z, *kind) : m_world.plantNaturalBush(m_terrain, x, z);
    }
    std::size_t eraseBushes(float x, float z, float radius) { return m_world.removeBushes(x, z, radius); }
    // Валун падает с высоты на (x, z), катится по рельефу, давит существ и вмятывает кратеры (см. Application::update).
    bool boulder(float x, float z, float radius) {
        if (!m_physics) return false;
        m_physics->addSphere({x, m_terrain.heightAt(x, z) + 24.0f, z}, std::clamp(radius, 0.5f, 6.0f), 1.0f, {0.0f, -2.0f, 0.0f});
        return true;
    }
    [[nodiscard]] std::size_t countNear(float x, float z, float radius) const { return m_world.countCreatures(x, z, radius); }

    [[nodiscard]] std::vector<Terrain::Rect> takeDirty() { return std::exchange(m_dirty, {}); }

private:
    World&                    m_world;
    Terrain&                  m_terrain;
    PhysicsWorld*             m_physics = nullptr;
    SoundSink*                m_sound = nullptr;
    std::vector<Terrain::Rect> m_dirty;
};

// Один вызов силы: точка под курсором, радиус кисти, шаг времени, и первый ли
// это вызов с момента нажатия кнопки (для сил с "запомнить исходную точку").
struct PowerCall {
    float x = 0, z = 0, radius = 0, dt = 0;
    bool  first = false;
};

struct PowerDef {
    std::string id;
    std::string name;
    std::string description;
    std::string source = "base";  // "base" или id мода
    float       radius = 8, minRadius = 2, maxRadius = 40;
    // Пока кнопка зажата: 0 -- каждый шаг симуляции, >0 -- раз в interval секунд,
    // <0 -- один раз на нажатие.
    float interval = 0;
    std::function<void(GodContext&, const PowerCall&)> apply;
};

// Данные-конструкторы: ими же собираются и встроенные силы, и силы модов из JSON.
[[nodiscard]] PowerDef makeRaisePower(std::string id, std::string name, float ratePerSecond);  // <0 -- опускает
[[nodiscard]] PowerDef makeFlattenPower(std::string id, std::string name, float ratePerSecond);
[[nodiscard]] PowerDef makeSpawnPower(std::string id, std::string name,
                                      std::vector<std::pair<Genome::TraitIndex, float>> traits, float interval);
[[nodiscard]] PowerDef makeErasePower(std::string id, std::string name);  // существа, постройки и кусты
[[nodiscard]] PowerDef makeBuildPower(std::string id, std::string name);   // дом по клику
// Ягодные кусты: в круге под кисточкой сажает кусты (пока кнопка зажата). kind пуст -- вид по биому каждой точки.
[[nodiscard]] PowerDef makeBushPower(std::string id, std::string name, std::optional<BerryKind> kind, float interval);
// Осмотр: выбор существа под курсором (обрабатывает Application -- панель состояния); сам мир не меняет.
[[nodiscard]] PowerDef makeInspectPower(std::string id, std::string name);
// Валуны: размер зависит от радиуса кисти (radius * 0.18), несколько в секунду пока кнопка зажата.
[[nodiscard]] PowerDef makeBoulderPower(std::string id, std::string name, float interval);

[[nodiscard]] std::vector<PowerDef> makeBuiltinPowers();

class PowerRegistry {
public:
    // false -- id уже занят (мод не может подменить чужую силу).
    bool add(PowerDef def);
    void removeFromSource(const std::string& source);
    [[nodiscard]] std::span<const PowerDef> all() const noexcept { return m_powers; }
    [[nodiscard]] const PowerDef* find(const std::string& id) const noexcept;

private:
    std::vector<PowerDef> m_powers;
};

}  // namespace wsim

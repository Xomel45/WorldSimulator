#include "sim/Powers.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>

namespace wsim {

PowerDef makeRaisePower(std::string id, std::string name, float ratePerSecond) {
    PowerDef d;
    d.id          = std::move(id);
    d.name        = std::move(name);
    d.description = ratePerSecond >= 0 ? "Поднимает рельеф под курсором" : "Опускает рельеф под курсором";
    d.radius      = 10;
    d.apply       = [ratePerSecond](GodContext& g, const PowerCall& c) { g.raise(c.x, c.z, c.radius, ratePerSecond * c.dt); };
    return d;
}

PowerDef makeFlattenPower(std::string id, std::string name, float ratePerSecond) {
    PowerDef d;
    d.id          = std::move(id);
    d.name        = std::move(name);
    d.description = "Выравнивает рельеф по высоте точки, с которой начато нажатие";
    d.radius      = 10;
    // Высота запоминается на первом вызове нажатия: за время мазка кисть тянет всё к одному уровню.
    auto target = std::make_shared<float>(0.0f);
    d.apply     = [ratePerSecond, target](GodContext& g, const PowerCall& c) {
        if (c.first) *target = g.height(c.x, c.z);
        g.flatten(c.x, c.z, c.radius, *target, ratePerSecond * c.dt);
    };
    return d;
}

PowerDef makeSpawnPower(std::string id, std::string name, std::vector<std::pair<Genome::TraitIndex, float>> traits,
                        float interval) {
    PowerDef d;
    d.id          = std::move(id);
    d.name        = std::move(name);
    d.description = "Создаёт существ в области под курсором";
    d.radius      = 4;
    d.minRadius   = 0.5f;
    d.maxRadius   = 20;
    d.interval    = interval;
    auto rng      = std::make_shared<std::mt19937>(std::random_device{}());
    d.apply       = [traits = std::move(traits), rng](GodContext& g, const PowerCall& c) {
        std::uniform_real_distribution<float> ang(0.0f, 6.2831853f), rad(0.0f, 1.0f);
        const float a = ang(*rng), r = c.radius * std::sqrt(rad(*rng));
        g.spawn(c.x + std::cos(a) * r, c.z + std::sin(a) * r, traits);
    };
    return d;
}

PowerDef makeErasePower(std::string id, std::string name) {
    PowerDef d;
    d.id          = std::move(id);
    d.name        = std::move(name);
    d.description = "Убирает существ, зверей, постройки и ягодные кусты в области под курсором";
    d.radius      = 6;
    d.minRadius   = 1;
    d.apply       = [](GodContext& g, const PowerCall& c) {
        g.erase(c.x, c.z, c.radius);
        g.eraseBuildings(c.x, c.z, c.radius);
        g.eraseBushes(c.x, c.z, c.radius);
        g.eraseAnimals(c.x, c.z, c.radius);
    };
    return d;
}

PowerDef makeBushPower(std::string id, std::string name, std::optional<BerryKind> kind, float interval) {
    PowerDef d;
    d.id          = std::move(id);
    d.name        = std::move(name);
    d.description = kind ? std::string("Сажает кусты: ") + berryName(*kind) + " (в круге под кистью, пока кнопка зажата)"
                         : "Сажает ягодные кусты; вид зависит от биома под курсором";
    d.radius      = 4;
    d.minRadius   = 0.5f;
    d.maxRadius   = 14;
    d.interval    = interval;
    auto rng      = std::make_shared<std::mt19937>(std::random_device{}());
    d.apply       = [kind, rng](GodContext& g, const PowerCall& c) {
        std::uniform_real_distribution<float> ang(0.0f, 6.2831853f), rad(0.0f, 1.0f);
        const float a = ang(*rng), r = c.radius * std::sqrt(rad(*rng));
        g.bush(c.x + std::cos(a) * r, c.z + std::sin(a) * r, kind);
    };
    return d;
}

PowerDef makeAnimalPower(std::string id, std::string name, std::optional<Species> species, float interval) {
    PowerDef d;
    d.id          = std::move(id);
    d.name        = std::move(name);
    d.description = species ? std::string("Выпускает зверей: ") + speciesDef(*species).name + " (в круге под кистью, пока кнопка зажата)"
                            : "Выпускает диких зверей; вид зависит от биома под курсором";
    d.radius      = 5;
    d.minRadius   = 0.5f;
    d.maxRadius   = 20;
    d.interval    = interval;
    auto rng      = std::make_shared<std::mt19937>(std::random_device{}());
    d.apply       = [species, rng](GodContext& g, const PowerCall& c) {
        std::uniform_real_distribution<float> ang(0.0f, 6.2831853f), rad(0.0f, 1.0f);
        const float a = ang(*rng), r = c.radius * std::sqrt(rad(*rng));
        g.animal(c.x + std::cos(a) * r, c.z + std::sin(a) * r, species);
    };
    return d;
}

PowerDef makeBuildPower(std::string id, std::string name) {
    PowerDef d;
    d.id          = std::move(id);
    d.name        = std::move(name);
    d.description = "Ставит дом на суше и выравнивает под ним площадку (один дом на клик)";
    d.radius = d.minRadius = d.maxRadius = 4;
    d.interval    = -1;
    auto rng      = std::make_shared<std::mt19937>(std::random_device{}());
    d.apply       = [rng](GodContext& g, const PowerCall& c) {
        std::uniform_real_distribution<float> yaw(0.0f, 360.0f);
        g.build(c.x, c.z, yaw(*rng));
    };
    return d;
}

PowerDef makeInspectPower(std::string id, std::string name) {
    PowerDef d;
    d.id          = std::move(id);
    d.name        = std::move(name);
    d.description = "Клик по существу: что оно делает, его нужды, здоровье и черты";
    d.radius      = 4;
    d.minRadius   = 1.5f;
    d.maxRadius   = 12;
    d.interval    = -1;
    d.apply       = [](GodContext&, const PowerCall&) {};  // выбор делает Application (нужен доступ к населению и интерфейсу)
    return d;
}

PowerDef makeBoulderPower(std::string id, std::string name, float interval) {
    PowerDef d;
    d.id          = std::move(id);
    d.name        = std::move(name);
    d.description = "Сбрасывает валуны: катятся по склонам, давят существ, оставляют кратеры";
    d.radius      = 10;
    d.minRadius   = 4;
    d.maxRadius   = 30;
    d.interval    = interval;
    auto rng      = std::make_shared<std::mt19937>(std::random_device{}());
    d.apply       = [rng](GodContext& g, const PowerCall& c) {
        std::uniform_real_distribution<float> ang(0.0f, 6.2831853f), rad(0.0f, 1.0f);
        const float a = ang(*rng), r = c.radius * 0.4f * std::sqrt(rad(*rng));
        g.boulder(c.x + std::cos(a) * r, c.z + std::sin(a) * r, c.radius * 0.18f);
    };
    return d;
}

std::vector<PowerDef> makeBuiltinPowers() {
    std::vector<PowerDef> v;
    v.push_back(makeRaisePower("raise", "Поднять", 8.0f));
    v.push_back(makeRaisePower("lower", "Опустить", -8.0f));
    v.push_back(makeFlattenPower("flatten", "Выровнять", 6.0f));
    v.push_back(makeSpawnPower("spawn", "Существа", {}, 0.12f));
    v.push_back(makeAnimalPower("animals", "Звери", std::nullopt, 0.6f));
    v.push_back(makeBoulderPower("boulder", "Валун", 0.45f));
    v.push_back(makeBuildPower("house", "Дом"));
    v.push_back(makeBushPower("bush", "Кусты", std::nullopt, 0.3f));
    v.push_back(makeInspectPower("inspect", "Осмотр"));
    v.push_back(makeErasePower("erase", "Стереть"));
    return v;
}

bool PowerRegistry::add(PowerDef def) {
    if (def.id.empty() || find(def.id)) return false;
    def.minRadius = std::max(0.5f, def.minRadius);
    def.maxRadius = std::max(def.minRadius, def.maxRadius);
    def.radius    = std::clamp(def.radius, def.minRadius, def.maxRadius);
    m_powers.push_back(std::move(def));
    return true;
}

void PowerRegistry::removeFromSource(const std::string& source) {
    std::erase_if(m_powers, [&](const PowerDef& p) { return p.source == source; });
}

const PowerDef* PowerRegistry::find(const std::string& id) const noexcept {
    for (const auto& p : m_powers)
        if (p.id == id) return &p;
    return nullptr;
}

}  // namespace wsim

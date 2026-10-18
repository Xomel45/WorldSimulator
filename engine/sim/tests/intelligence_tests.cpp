#include "sim/World.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace wsim;

namespace {

constexpr float kDt = 1.0f / 20.0f;

// Мир с заданным составом: берём настоящий World (ГСЧ, мир) и подменяем население и еду.
World scenario(std::uint32_t seed, std::vector<Creature> creatures, std::vector<FoodSource> food) {
    World w(seed);
    WorldState st = w.exportState();
    st.population.clear();
    std::uint64_t id = 1;
    for (auto& c : creatures) {
        if (c.id == 0) c.id = id++;
        st.population.push_back({std::move(c), BrainOutputs{}});
    }
    st.nextId = id + 100;
    st.food   = std::move(food);
    st.buildings.clear();
    st.pendingGrief.clear();
    WSIM_CHECK(w.importState(st));
    return w;
}

Creature adult(std::uint32_t seed, glm::vec3 pos) {
    Rng rng(seed);
    Creature c;
    c.genome   = Genome::random(rng);
    c.position = pos;
    c.age      = c.lifespan() * 0.3f;  // взрослый
    c.needs.hunger = 0.2f;
    return c;
}

}  // namespace

int main() {
    // ── Экология: за несколько минут популяция не взрывается и не вымирает, начинается отбор ──
    for (std::uint32_t seed : {5u, 11u}) {
        Terrain terrain(World::kWorldHalfSize, seed);
        World   w(seed);
        w.settleOnLand(terrain);

        std::size_t popAt30 = 0, peak = 0;
        double hungerSum = 0;
        long   samples = 0;
        const int steps = static_cast<int>(300.0f / kDt);  // 5 минут
        for (int i = 0; i < steps; ++i) {
            w.tick(kDt, &terrain);
            const float t = static_cast<float>(i) * kDt;
            if (i == static_cast<int>(30.0f / kDt)) popAt30 = w.populationCount();
            peak = std::max(peak, w.populationCount());
            if (i % 20 == 0)
                for (const auto& in : w.population()) { hungerSum += in.creature.needs.hunger; ++samples; }

            // Дети не размножаются до зрелости -- иначе лавина (это и была причина 24 -> 200 за секунду).
            if (i % 100 == 0)
                for (const auto& in : w.population())
                    if (!in.creature.isMature()) WSIM_CHECK(in.creature.childIds.empty());
        }
        WSIM_CHECK(popAt30 <= 40);                       // за первые 30 с -- не лавина
        WSIM_CHECK(w.populationCount() >= 8);            // не вымерли
        WSIM_CHECK(peak < World::kMaxPopulation);        // упора в потолок нет
        WSIM_CHECK(hungerSum / static_cast<double>(samples) < 0.75);   // в среднем кормятся
        const WorldStats& st = w.stats();
        WSIM_CHECK(st.births > 0 && st.matings >= st.births);
        WSIM_CHECK(st.deathsStarved > 0);                // еда ограничена -- голод реально убивает
    }

    // ── Не слипаются: через несколько минут нет кучи существ друг в друге ──
    {
        Terrain terrain(World::kWorldHalfSize, 29);
        World   w(29);
        w.settleOnLand(terrain);
        for (int i = 0; i < static_cast<int>(180.0f / kDt); ++i) w.tick(kDt, &terrain);
        const auto pop = w.population();
        int stacked = 0;
        for (std::size_t i = 0; i < pop.size(); ++i)
            for (std::size_t j = 0; j < pop.size(); ++j)
                if (i != j && glm::length(pop[i].creature.position - pop[j].creature.position) < 0.4f) { ++stacked; break; }
        WSIM_CHECK(static_cast<double>(stacked) <= 0.15 * static_cast<double>(pop.size()));
    }

    // ── Голодный идёт к еде и ест ──
    {
        Creature c = adult(3, {0.0f, 0.0f, 0.0f});
        c.needs.hunger = 0.7f;
        World w = scenario(3, {c}, {FoodSource{{18.0f, 0.0f, 6.0f}, 1.0f}});
        const float h0 = w.population()[0].creature.needs.hunger;
        bool  sawSeek = false, sawEat = false;
        float minHunger = h0, nearestToFood = 1e9f;
        for (int i = 0; i < static_cast<int>(40.0f / kDt); ++i) {
            w.tick(kDt, nullptr);
            const Creature& cr = w.population()[0].creature;
            sawSeek |= cr.behavior == Behavior::SeekFood;
            sawEat  |= cr.behavior == Behavior::Eat;
            minHunger = std::min(minHunger, cr.needs.hunger);
            nearestToFood = std::min(nearestToFood, glm::length(glm::vec2(cr.position.x - 18.0f, cr.position.z - 6.0f)));
        }
        WSIM_CHECK(sawSeek && sawEat);
        WSIM_CHECK(minHunger < h0 - 0.5f);   // наелся (потом территориальное может увести домой -- это нормально)
        WSIM_CHECK(nearestToFood < 3.0f);    // дошёл до куста
    }

    // ── Помнит, где была еда: еда пропала из виду (ушёл далеко), но голодный возвращается к месту ──
    {
        Creature c = adult(4, {0.0f, 0.0f, 0.0f});
        c.needs.hunger = 0.6f;
        c.foodMemory    = {-40.0f, 0.0f, 0.0f};  // видел давно, далеко (за пределами зрения)
        c.foodMemoryAge = 5.0f;
        World w = scenario(4, {c}, {});
        const float d0 = std::fabs(w.population()[0].creature.position.x - (-40.0f));
        for (int i = 0; i < static_cast<int>(15.0f / kDt); ++i) w.tick(kDt, nullptr);
        const float d1 = std::fabs(w.population()[0].creature.position.x - (-40.0f));
        WSIM_CHECK(d1 < d0 - 8.0f);  // заметно приблизился к запомненному месту
    }

    // ── Устал -- останавливается и отдыхает, энергия растёт ──
    {
        Creature c = adult(5, {0.0f, 0.0f, 0.0f});
        c.needs.energy = 0.12f;
        c.needs.hunger = 0.1f;
        World w = scenario(5, {c}, {});
        bool resting = false;
        for (int i = 0; i < static_cast<int>(20.0f / kDt); ++i) {
            w.tick(kDt, nullptr);
            if (w.population()[0].creature.behavior == Behavior::Rest) resting = true;
        }
        WSIM_CHECK(resting);
        WSIM_CHECK(w.population()[0].creature.needs.energy > 0.12f + 0.3f);
    }

    // ── Бежит от опасности: существа вокруг валуна расходятся ──
    {
        std::vector<Creature> group;
        for (int k = 0; k < 8; ++k) group.push_back(adult(10 + k, {6.0f * std::cos(k * 0.78f), 0.0f, 6.0f * std::sin(k * 0.78f)}));
        for (auto& c : group) c.genome.setTrait(Genome::kBoldness, -1.0f);  // трусы: реакция однозначна
        World w = scenario(6, group, {});
        const Hazard h{{0.0f, 0.0f, 0.0f}, 2.0f};
        auto meanDist = [&] {
            double d = 0;
            for (const auto& in : w.population()) d += glm::length(glm::vec2(in.creature.position.x, in.creature.position.z));
            return d / static_cast<double>(w.populationCount());
        };
        const double before = meanDist();
        bool fled = false;
        for (int i = 0; i < static_cast<int>(6.0f / kDt); ++i) {
            w.setHazards(std::span<const Hazard>(&h, 1));
            w.tick(kDt, nullptr);
            for (const auto& in : w.population()) if (in.creature.behavior == Behavior::Flee) fled = true;
        }
        WSIM_CHECK(fled);
        WSIM_CHECK(meanDist() > before + 6.0);  // разбежались от центра

        // Опасность убрали -- паника проходит, к концу минуты никто не бежит.
        for (int i = 0; i < static_cast<int>(60.0f / kDt); ++i) { w.setHazards({}); w.tick(kDt, nullptr); }
        int fleeing = 0;
        for (const auto& in : w.population()) if (in.creature.behavior == Behavior::Flee) ++fleeing;
        WSIM_CHECK(fleeing == 0);
    }

    // ── Готовая пара находит друг друга и размножается; неготовая (голодная) -- нет ──
    {
        auto pairWorld = [&](float hunger) {
            Creature a = adult(20, {0.0f, 0.0f, 0.0f}), b = adult(21, {9.0f, 0.0f, 0.0f});
            for (Creature* c : {&a, &b}) {
                c->needs.libido = 0.9f;
                c->needs.hunger = hunger;
                c->needs.energy = 0.95f;
                c->genome.setTrait(Genome::kFertility, 1.0f);
            }
            return scenario(7, {a, b}, {});
        };
        World ready = pairWorld(0.15f);
        for (int i = 0; i < static_cast<int>(40.0f / kDt); ++i) ready.tick(kDt, nullptr);
        WSIM_CHECK(ready.stats().births >= 1);

        World hungry = pairWorld(0.9f);
        for (int i = 0; i < static_cast<int>(10.0f / kDt); ++i) hungry.tick(kDt, nullptr);
        WSIM_CHECK(hungry.stats().births == 0);
    }

    std::puts("intelligence_tests: OK");
    return 0;
}

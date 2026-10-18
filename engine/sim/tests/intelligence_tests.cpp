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

    // ── Ягоды: вкус запоминается, отравившись, существо обходит ядовитый куст и идёт к съедобному ──
    {
        // Единственный куст -- волчья ягода: пробует, чувствует яд, бросает; не умирает, не объедает куст целиком.
        Creature c = adult(3, {0.0f, 0.0f, 0.0f});
        c.needs.hunger = 0.5f;
        World w = scenario(3, {c}, {FoodSource{{4.0f, 0.0f, 2.0f}, 1.0f, BerryKind::Nightshade}});
        float maxToxin = 0.0f;
        for (int i = 0; i < static_cast<int>(25.0f / kDt); ++i) {
            w.tick(kDt, nullptr);
            if (!w.population().empty()) maxToxin = std::max(maxToxin, w.population()[0].creature.toxin);
        }
        WSIM_CHECK(w.population().size() == 1);                       // выжило
        const Creature& cr = w.population()[0].creature;
        WSIM_CHECK(cr.knowsBerry(BerryKind::Nightshade));             // пробовало
        WSIM_CHECK(cr.berryAppeal(BerryKind::Nightshade) < Creature::kMinAppeal);  // и запомнило, что это плохо
        WSIM_CHECK(maxToxin > 0.02f && maxToxin < 1.3f);              // отравилось, но не смертельно
        WSIM_CHECK(w.food()[0].amount > 0.5f);                        // куст не съеден целиком: бросило после пары укусов
    }
    {
        // Рядом волчья ягода, подальше морошка: сначала пробует ближнюю, отравляется и уходит к морошке, наедается.
        Creature c = adult(3, {0.0f, 0.0f, 0.0f});
        c.needs.hunger = 0.8f;
        World w = scenario(3, {c}, {FoodSource{{3.0f, 0.0f, 0.0f}, 1.0f, BerryKind::Nightshade}, FoodSource{{14.0f, 0.0f, 5.0f}, 1.0f, BerryKind::Cloudberry}});
        float minHunger = 1.0f, minCloud = 1.0f;
        for (int i = 0; i < static_cast<int>(90.0f / kDt); ++i) {
            w.tick(kDt, nullptr);
            if (!w.population().empty()) minHunger = std::min(minHunger, w.population()[0].creature.needs.hunger);
            minCloud = std::min(minCloud, w.food()[1].amount);
        }
        WSIM_CHECK(w.population().size() == 1);
        const Creature& cr = w.population()[0].creature;
        WSIM_CHECK(cr.knowsBerry(BerryKind::Nightshade) && cr.berryAppeal(BerryKind::Nightshade) < 0.5f);  // пары укусов хватило, чтобы перестать верить этому кусту
        WSIM_CHECK(cr.knowsBerry(BerryKind::Cloudberry) && cr.berryAppeal(BerryKind::Cloudberry) > 1.0f);
        WSIM_CHECK(minHunger < 0.35f);                                 // в итоге наелось съедобным
        WSIM_CHECK(minCloud < 0.7f);                                      // морошку ели (потом куст отрастает)
    }
    {
        // Недомогания по видам: каждый ядовитый куст вызывает именно своё и (кроме волчьей ягоды) не убивает.
        for (BerryKind kind : {BerryKind::Buckthorn, BerryKind::Honeysuckle, BerryKind::Henbane, BerryKind::Baneberry}) {
            const BerryDef& def = berryDef(kind);
            Creature c = adult(3, {0.0f, 0.0f, 0.0f});
            c.needs.hunger = 0.6f;
            World w = scenario(3, {c}, {FoodSource{{3.0f, 0.0f, 2.0f}, 1.0f, kind}});
            float maxDiarrhea = 0, maxNausea = 0, maxHallu = 0, maxToxin = 0;
            bool  vomited = false;
            for (int i = 0; i < static_cast<int>(40.0f / kDt) && w.populationCount() > 0; ++i) {
                w.tick(kDt, nullptr);
                const Creature& cr = w.population()[0].creature;
                maxDiarrhea = std::max(maxDiarrhea, cr.diarrhea);
                maxNausea   = std::max(maxNausea, cr.nausea);
                maxHallu    = std::max(maxHallu, cr.hallucination);
                maxToxin    = std::max(maxToxin, cr.toxin);
                vomited |= cr.behavior == Behavior::Sick;
            }
            WSIM_CHECK(w.populationCount() == 1 && maxToxin == 0.0f);        // смертельного ничего нет
            WSIM_CHECK((maxDiarrhea > 0.05f) == (def.diarrhea > 0.0f));       // понос -- только у тех, у кого он есть
            WSIM_CHECK((maxHallu > 0.05f) == (def.hallucination > 0.0f));
            WSIM_CHECK((maxNausea > 0.05f) == (def.vomit > 0.0f));
            if (def.vomit > 0.0f) WSIM_CHECK(vomited || maxNausea < 1.0f);    // тошнота, дойдя до порога, кончается рвотой
        }
    }
    {
        // Галлюцинирующий зовёт на помощь без причины -- и соседи пугаются по-настоящему (паника по стае).
        auto maxNeighbourFear = [&](float hallu) {
            Creature a = adult(3, {0.0f, 0.0f, 0.0f}), b = adult(4, {5.0f, 0.0f, 0.0f});
            a.needs.hunger = b.needs.hunger = 0.1f;
            a.hallucination = hallu;
            World w = scenario(3, {a, b}, {});
            float maxFear = 0.0f;
            for (int i = 0; i < static_cast<int>(30.0f / kDt); ++i) {
                w.tick(kDt, nullptr);
                for (const auto& in : w.population())
                    if (in.creature.id == 2) maxFear = std::max(maxFear, in.creature.needs.fear);
            }
            return maxFear;
        };
        WSIM_CHECK(maxNeighbourFear(2.0f) > maxNeighbourFear(0.0f) + 0.05f);
    }
    {
        // Социальное обучение (плохое): сосед отравился на моих глазах -- запоминаю эту ягоду, сам не пробуя. Далёкий не видел.
        auto observerAppeal = [&](float distance, bool kin) {
            Creature eater = adult(3, {0.0f, 0.0f, 0.0f}), watcher = adult(4, {distance, 0.0f, 0.0f}), far = adult(5, {90.0f, 0.0f, 90.0f});
            eater.id = 1; watcher.id = 2; far.id = 3;
            eater.needs.hunger = 0.8f;
            watcher.needs.hunger = far.needs.hunger = 0.05f;   // сытые: к кусту не пойдут
            if (kin) watcher.parentIds[0] = 1;
            World w = scenario(3, {eater, watcher, far}, {FoodSource{{2.0f, 0.0f, 1.0f}, 1.0f, BerryKind::Nightshade}});
            for (int i = 0; i < static_cast<int>(20.0f / kDt); ++i) w.tick(kDt, nullptr);
            struct R { float watcher, far, prior; bool watcherAte, farKnows; } r{};
            r.prior = watcher.berryAppeal(BerryKind::Nightshade);
            for (const auto& in : w.population()) {
                if (in.creature.id == 2) { r.watcher = in.creature.berryAppeal(BerryKind::Nightshade); r.watcherAte = in.creature.sinceBite < 999.0f; }
                if (in.creature.id == 3) { r.far = in.creature.berryAppeal(BerryKind::Nightshade); r.farKnows = in.creature.knowsBerry(BerryKind::Nightshade); }
            }
            return r;
        };
        const auto near = observerAppeal(6.0f, true);
        WSIM_CHECK(!near.watcherAte);                       // сам ничего не ел
        WSIM_CHECK(near.watcher < near.prior - 0.3f);       // но по чужой беде "тёмные" ягоды вызывают у него недоверие
        WSIM_CHECK(!near.farKnows && near.far >= 0.6f - 1e-4f);  // далёкий ничего не знает
        const auto stranger = observerAppeal(6.0f, false);
        WSIM_CHECK(stranger.watcher > near.watcher && stranger.watcher < stranger.prior);  // чужому верят меньше, чем родне, но верят
    }
    {
        // Социальное обучение (хорошее): спокойно ест морошку рядом -- незнакомая ягода меньше пугает, чем была.
        Creature eater = adult(3, {0.0f, 0.0f, 0.0f}), watcher = adult(4, {5.0f, 0.0f, 0.0f});
        eater.id = 1; watcher.id = 2;
        eater.needs.hunger = 0.8f;
        watcher.needs.hunger = 0.05f;
        const float before = watcher.berryAppeal(BerryKind::Cloudberry);
        World w = scenario(3, {eater, watcher}, {FoodSource{{2.0f, 0.0f, 1.0f}, 1.0f, BerryKind::Cloudberry}});
        for (int i = 0; i < static_cast<int>(25.0f / kDt); ++i) w.tick(kDt, nullptr);
        for (const auto& in : w.population())
            if (in.creature.id == 2) {
                WSIM_CHECK(in.creature.knowsBerry(BerryKind::Cloudberry) && in.creature.berryAppeal(BerryKind::Cloudberry) > before + 0.1f);
                WSIM_CHECK(in.creature.sinceBite > 999.0f);  // сам не ел
            }
    }
    {
        // Зуб: завистливый с обидой замечает самую заметную демонстрацию статуса рядом и берёт на заметку.
        Creature envious = adult(3, {0.0f, 0.0f, 0.0f}), show = adult(4, {6.0f, 0.0f, 0.0f});
        envious.id = 1; show.id = 2;
        envious.genome.setTrait(Genome::kEnvy, 1.0f);
        envious.needs.resentment = 0.8f;
        World w = scenario(3, {envious, show}, {});
        WorldState st = w.exportState();
        st.population[1].lastOutputs.dominanceDisplay = 0.9f;  // сосед выставляет себя напоказ
        WSIM_CHECK(w.importState(st));
        w.tick(kDt, nullptr);
        const Creature& e = w.population()[0].creature;
        WSIM_CHECK(e.grudgeId == 2 && e.grudge > 0.0f);
        // Без обиды (envy = 0) зуба не бывает.
        Creature meek = adult(3, {0.0f, 0.0f, 0.0f});
        meek.id = 1;
        meek.genome.setTrait(Genome::kEnvy, -1.0f);
        meek.needs.resentment = 0.8f;
        World w2 = scenario(3, {meek, show}, {});
        WorldState st2 = w2.exportState();
        st2.population[1].lastOutputs.dominanceDisplay = 0.9f;
        WSIM_CHECK(w2.importState(st2));
        for (int i = 0; i < 60; ++i) w2.tick(kDt, nullptr);
        WSIM_CHECK(w2.population()[0].creature.grudgeId == 0);
    }
    {
        // Месть: с зубом на соседа, зная, что "тёмные" ягоды плохие, идёт к такому кусту, берёт ягоды и подсовывает обидчику.
        Creature avenger = adult(3, {0.0f, 0.0f, 0.0f}), victim = adult(4, {14.0f, 0.0f, 0.0f});
        avenger.id = 1; victim.id = 2;
        avenger.genome.setTrait(Genome::kEmpathy, -1.0f);
        avenger.needs.hunger = 0.1f;
        victim.needs.hunger = 0.05f;
        avenger.grudgeId = 2;
        avenger.grudge   = 0.95f;
        const auto dark = static_cast<std::size_t>(BerryLook::Dark);
        avenger.taste[dark] = -1.0f;
        avenger.tasteSamples[dark] = 50;
        World w = scenario(3, {avenger, victim}, {FoodSource{{6.0f, 0.0f, 2.0f}, 1.0f, BerryKind::Nightshade}});
        float maxToxinVictim = 0.0f;
        bool  sawSabotage = false;
        for (int i = 0; i < static_cast<int>(40.0f / kDt); ++i) {
            w.tick(kDt, nullptr);
            for (const auto& in : w.population()) {
                if (in.creature.id == 2) maxToxinVictim = std::max(maxToxinVictim, in.creature.toxin);
                if (in.creature.id == 1) sawSabotage |= in.creature.behavior == Behavior::Sabotage;
            }
        }
        WSIM_CHECK(sawSabotage);
        WSIM_CHECK(w.stats().sabotages >= 1);          // подсунул
        WSIM_CHECK(maxToxinVictim > 0.1f);             // жертва получила дозу волчьей ягоды
        WSIM_CHECK(w.food()[0].amount < 0.95f || w.stats().sabotages >= 1);
        for (const auto& in : w.population()) {
            if (in.creature.id == 1) WSIM_CHECK(in.creature.grudge < 0.5f && in.creature.carryAmount == 0.0f);  // отомстил -- отпустило
            if (in.creature.id == 2) WSIM_CHECK(in.creature.knowsBerry(BerryKind::Nightshade));                   // а жертва запомнила вкус
        }
        // Эмпатичный на такое не идёт.
        Creature kind = avenger;
        kind.genome.setTrait(Genome::kEmpathy, 1.0f);
        World w3 = scenario(3, {kind, victim}, {FoodSource{{6.0f, 0.0f, 2.0f}, 1.0f, BerryKind::Nightshade}});
        for (int i = 0; i < static_cast<int>(40.0f / kDt); ++i) w3.tick(kDt, nullptr);
        WSIM_CHECK(w3.stats().sabotages == 0);
    }
    {
        // Статистика: смерть от яда считается отдельно.
        Creature c = adult(3, {0.0f, 0.0f, 0.0f});
        c.needs.hunger = 0.3f;
        c.health = 0.05f;
        c.addToxin(2.0f);
        World w = scenario(3, {c}, {});
        for (int i = 0; i < static_cast<int>(20.0f / kDt) && w.populationCount() > 0; ++i) w.tick(kDt, nullptr);
        WSIM_CHECK(w.populationCount() == 0 && w.stats().deathsPoison == 1);
    }

    std::puts("intelligence_tests: OK");
    return 0;
}

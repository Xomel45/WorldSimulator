// "Умный мозг": топология, память, влияние желаний-выборов на поведение, эволюция сети и миграция старых сохранений.
#include "sim/Behavior.hpp"
#include "sim/Brain.hpp"
#include "sim/Creature.hpp"
#include "sim/Genome.hpp"
#include "sim/SaveGame.hpp"
#include "sim/Terrain.hpp"
#include "sim/World.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace wsim;

int main() {
    // Топология: 47 входов + 8 памяти, 32 скрытых, 16 выходов (нейронов: 47 + 32 + 16 = 95), 2320 весов + 13 черт.
    static_assert(brain::kInputCount == 55 && brain::kHiddenCount == 32 && brain::kOutputCount == 16 && brain::kMemoryCount == 8);
    static_assert(Genome::kGeneCount == 13 + 2320);
    static_assert(brain::kWeightCount == 2320);

    // Память: прошлая активация входа памяти меняет выход (рекуррентная связь работает).
    {
        std::vector<float> fixed(static_cast<std::size_t>(brain::kInputHiddenWeights + brain::kHiddenCount), 0.0f);
        std::vector<float> plastic(static_cast<std::size_t>(brain::kHiddenOutputWeights + brain::kOutputCount), 0.0f);
        const int memInput = brain::kInputCount - brain::kMemoryCount;   // первый вход памяти
        fixed[static_cast<std::size_t>(0 * brain::kInputCount + memInput)] = 1.0f;     // memory[0] -> hidden 0
        plastic[static_cast<std::size_t>(0 * brain::kHiddenCount + 0)] = 2.0f;         // hidden 0 -> moveX
        BrainInputs a{}, b{};
        b.memory[0] = 1.0f;
        const float outA = brain::evaluate(fixed, plastic, a).outputs.moveX, outB = brain::evaluate(fixed, plastic, b).outputs.moveX;
        WSIM_CHECK(outB > outA + 0.5f);
        const brain::BrainEvalResult r = brain::evaluate(fixed, plastic, b);
        WSIM_CHECK(r.hiddenActivations.size() == static_cast<std::size_t>(brain::kHiddenCount) && r.outputActivations.size() == static_cast<std::size_t>(brain::kOutputCount));
    }

    // Желания-выборы реально решают: при равных условиях охотничье желание 1 берёт охоту, 0 -- нет (идёт к ягодам).
    {
        auto chosen = [&](float huntDesire) {
            Rng rng(12);
            Creature c;
            c.genome = Genome::random(rng);
            c.age = c.lifespan() * 0.3f;
            c.needs.hunger = 0.55f;
            c.spearDur = 1.0f;
            c.skills = kSkillSpear;
            SenseInputs s;
            s.preyDist = 8.0f; s.preyPos = {8.0f, 0.0f, 0.0f};
            s.foodDist = 15.0f; s.foodPos = {0.0f, 0.0f, 15.0f};   // ягоды видны, но дальше добычи
            BrainOutputs raw{};
            raw.huntDesire = huntDesire;
            raw.eatDesire = raw.fleeDesire = raw.restDesire = 0.5f;   // нейтральные прежние желания
            for (int i = 0; i < 30; ++i) (void)behavior::decide(c, s, raw, 0.05f);
            return c.behavior;
        };
        WSIM_CHECK(chosen(1.0f) == Behavior::Hunt);
        WSIM_CHECK(chosen(0.0f) != Behavior::Hunt);
    }

    // Эволюция: ГА с кроссовером и мутацией учит сеть "охотничье желание = близость добычи" (новые вход и выход) -- ошибка падает вдвое.
    {
        Rng rng(2024);
        constexpr int kPop = 48, kGens = 40, kSamples = 24;
        std::vector<float> proximities(kSamples);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        for (float& p : proximities) p = u(rng);
        auto error = [&](const Genome& g) {
            float e = 0.0f;
            for (float p : proximities) {
                BrainInputs in{};
                in.preyProximity = p;
                const float desire = brain::evaluate(g.fixedBrainWeights(), g.initialPlasticWeights(), in).outputs.huntDesire;
                e += (desire - p) * (desire - p);
            }
            return e / static_cast<float>(kSamples);
        };
        std::vector<Genome> pop;
        for (int i = 0; i < kPop; ++i) pop.push_back(Genome::random(rng));
        float first = 1e9f, last = 1e9f;
        for (int gen = 0; gen < kGens; ++gen) {
            std::vector<std::pair<float, std::size_t>> rank;
            for (std::size_t i = 0; i < pop.size(); ++i) rank.emplace_back(error(pop[i]), i);
            std::sort(rank.begin(), rank.end());
            if (gen == 0) first = rank[0].first;
            last = rank[0].first;
            std::vector<Genome> next;
            for (int i = 0; i < 8; ++i) next.push_back(pop[rank[static_cast<std::size_t>(i)].second]);   // элита
            while (next.size() < pop.size()) {
                const Genome& a = pop[rank[static_cast<std::size_t>(rng() % 16)].second];
                const Genome& b = pop[rank[static_cast<std::size_t>(rng() % 16)].second];
                Genome child = Genome::crossover(a, b, rng);
                child.mutate(rng, 0.05f, 0.15f);
                next.push_back(std::move(child));
            }
            pop = std::move(next);
        }
        std::printf("evolution: best error %.4f -> %.4f over %d generations\n", first, last, kGens);
        WSIM_CHECK(last < first * 0.5f);
    }

    // Мозг у существа заполняет память, а поколения растут у потомков.
    {
        Terrain t(World::kWorldHalfSize, 11);
        World w(11);
        w.settleOnLand(t);
        w.growBerryBushes(t);
        w.formTribes(t);
        for (int i = 0; i < 60 * 60 * 6; ++i) { const glm::vec2 f{0, 0}; w.setFocus(std::span<const glm::vec2>(&f, 1)); w.tick(1.0f / 60.0f, &t); (void)w.takeDirty(); }
        bool memory = false;
        for (const Inhabitant& in : w.population()) for (float m : in.creature.brainMemory) memory |= std::fabs(m) > 0.01f;
        WSIM_CHECK(memory);
        WSIM_CHECK(w.stats().births > 0 && w.maxGeneration() >= 1);
        // Сохранение несёт память и поколение.
        PhysicsWorld physics;
        SaveGame back;
        std::string err;
        WSIM_CHECK(parseSave(serializeSave(captureSave("b", 0, 0.0f, t, w, physics, CameraState{})), back, err));
        WSIM_CHECK(back.world.population.size() == w.populationCount());
        for (std::size_t i = 0; i < w.populationCount(); ++i) {
            WSIM_CHECK(back.world.population[i].creature.generation == w.population()[i].creature.generation);
            WSIM_CHECK(back.world.population[i].creature.brainMemory == w.population()[i].creature.brainMemory);
            WSIM_CHECK(back.world.population[i].creature.genome.genes().size() == Genome::kGeneCount);
        }
        // Старый формат (до "умного мозга", v14): грузится, черты сохранены, мозг новый, но одинаковый при каждой загрузке.
        SaveGame old1, old2;
        const auto bytes14 = serializeSave(captureSave("old", 0, 0.0f, t, w, physics, CameraState{}), 14);
        WSIM_CHECK(parseSave(bytes14, old1, err) && parseSave(bytes14, old2, err));
        WSIM_CHECK(old1.world.population.size() == w.populationCount());
        for (std::size_t i = 0; i < w.populationCount(); ++i) {
            const auto& a = old1.world.population[i].creature.genome;
            const auto& b = old2.world.population[i].creature.genome;
            WSIM_CHECK(a.genes().size() == Genome::kGeneCount && std::equal(a.genes().begin(), a.genes().end(), b.genes().begin()));
            for (std::size_t k = 0; k < Genome::kTraitGeneCount; ++k) WSIM_CHECK(a.genes()[k] == w.population()[i].creature.genome.genes()[k]);
        }
    }

    std::puts("smartbrain_tests: OK");
    return 0;
}

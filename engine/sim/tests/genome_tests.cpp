#include "sim/Genome.hpp"

#include "test_util.hpp"

#include <cstddef>
#include <cstdio>

using namespace wsim;

namespace {
constexpr Genome::TraitIndex kAllTraits[] = {
    Genome::kSpeed, Genome::kSize, Genome::kMetabolism, Genome::kLifespan, Genome::kFertility,
    Genome::kPlasticity, Genome::kBoldness, Genome::kSociability, Genome::kBondStrength, Genome::kEnvy,
    Genome::kTerritorial, Genome::kCuriosity, Genome::kEmpathy,
};
}

int main() {
    // Детерминизм: одинаковый сид -> одинаковый геном.
    Rng rngA(42);
    Genome a = Genome::random(rngA);
    Rng rngB(42);
    Genome b = Genome::random(rngB);

    for (auto i : kAllTraits) {
        WSIM_CHECK(a.trait(i) == b.trait(i));
        WSIM_CHECK(a.trait(i) >= -1.0f && a.trait(i) <= 1.0f);
    }

    auto weightsA = a.brainWeights();
    auto weightsB = b.brainWeights();
    WSIM_CHECK(weightsA.size() == weightsB.size());
    for (std::size_t i = 0; i < weightsA.size(); ++i) {
        WSIM_CHECK(weightsA[i] == weightsB[i]);
        WSIM_CHECK(weightsA[i] >= -1.0f && weightsA[i] <= 1.0f);
    }

    // fixedBrainWeights() + initialPlasticWeights() -- ровно та же память, что
    // и brainWeights(), без дыры/перехлёста между двумя половинами.
    {
        auto fixed   = a.fixedBrainWeights();
        auto plastic = a.initialPlasticWeights();
        WSIM_CHECK(fixed.size() + plastic.size() == weightsA.size());
        for (std::size_t i = 0; i < fixed.size(); ++i)
            WSIM_CHECK(fixed[i] == weightsA[i]);
        for (std::size_t i = 0; i < plastic.size(); ++i)
            WSIM_CHECK(plastic[i] == weightsA[fixed.size() + i]);
    }

    // Кроссовер: каждый ген ребёнка -- ровно от одного из родителей.
    Rng rngC(7);
    Genome c = Genome::random(rngC);
    Rng rngX(123);
    Genome child = Genome::crossover(a, c, rngX);

    for (auto i : kAllTraits) {
        const float g = child.trait(i);
        WSIM_CHECK(g == a.trait(i) || g == c.trait(i));
    }
    auto weightsChild = child.brainWeights();
    auto weightsC = c.brainWeights();
    for (std::size_t i = 0; i < weightsChild.size(); ++i)
        WSIM_CHECK(weightsChild[i] == weightsA[i] || weightsChild[i] == weightsC[i]);

    // Мутация никогда не выходит за [-1, 1], даже после многих применений.
    Rng rngM(99);
    Genome mutant = child;
    for (int iter = 0; iter < 1000; ++iter)
        mutant.mutate(rngM, 0.3f, 0.5f);

    for (auto i : kAllTraits)
        WSIM_CHECK(mutant.trait(i) >= -1.0f && mutant.trait(i) <= 1.0f);
    for (float w : mutant.brainWeights())
        WSIM_CHECK(w >= -1.0f && w <= 1.0f);

    std::puts("genome_tests: OK");
    return 0;
}

#include "sim/Genome.hpp"

#include "sim/Brain.hpp"

#include <algorithm>

namespace wsim {

// kBrainWeightCount/kFixedBrainWeightCount/kPlasticBrainWeightCount дублируют
// brain::kWeightCount и его разбивку (публичный заголовок Genome намеренно
// не включает Brain.hpp) — эти static_assert ловят рассинхрон при изменении
// топологии сети.
static_assert(Genome::kBrainWeightCount == static_cast<std::size_t>(brain::kWeightCount));
static_assert(Genome::kFixedBrainWeightCount ==
              static_cast<std::size_t>(brain::kInputHiddenWeights + brain::kHiddenCount));
static_assert(Genome::kPlasticBrainWeightCount ==
              static_cast<std::size_t>(brain::kHiddenOutputWeights + brain::kOutputCount));
static_assert(Genome::kFixedBrainWeightCount + Genome::kPlasticBrainWeightCount ==
              Genome::kBrainWeightCount);

std::optional<Genome::TraitIndex> Genome::traitFromName(std::string_view name) noexcept {
    struct Entry { std::string_view name; TraitIndex index; };
    static constexpr Entry kNames[] = {
        {"speed", kSpeed},         {"size", kSize},           {"metabolism", kMetabolism},
        {"lifespan", kLifespan},   {"fertility", kFertility}, {"plasticity", kPlasticity},
        {"boldness", kBoldness},   {"sociability", kSociability}, {"bondStrength", kBondStrength},
        {"envy", kEnvy},           {"territorial", kTerritorial}, {"curiosity", kCuriosity},
        {"empathy", kEmpathy},
    };
    for (const auto& e : kNames)
        if (e.name == name) return e.index;
    return std::nullopt;
}

Genome Genome::random(Rng& rng) {
    std::vector<float> genes(kGeneCount);

    std::uniform_real_distribution<float> traitDist(-1.0f, 1.0f);
    for (std::size_t i = 0; i < kTraitGeneCount; ++i)
        genes[i] = traitDist(rng);

    // Веса мозга — небольшая амплитуда вокруг нуля, а не полный [-1,1] шум:
    // крупные стартовые веса насыщают tanh/sigmoid почти до констант (мозг
    // "залипает" в одном решении вместо отклика на входы). Малые веса держат
    // нейроны в отзывчивой зоне — стандартная практика инициализации сетей,
    // здесь же играющая роль "разумных инстинктов по умолчанию".
    std::normal_distribution<float> weightDist(0.0f, 0.35f);
    for (std::size_t i = kTraitGeneCount; i < kGeneCount; ++i)
        genes[i] = std::clamp(weightDist(rng), -1.0f, 1.0f);

    return Genome(std::move(genes));
}

Genome Genome::crossover(const Genome& a, const Genome& b, Rng& rng) {
    std::vector<float> genes(kGeneCount);
    std::bernoulli_distribution coin(0.5);
    for (std::size_t i = 0; i < kGeneCount; ++i)
        genes[i] = coin(rng) ? a.m_genes[i] : b.m_genes[i];
    return Genome(std::move(genes));
}

void Genome::mutate(Rng& rng, float rate, float strength) {
    std::bernoulli_distribution shouldMutate(rate);
    std::normal_distribution<float> noise(0.0f, strength);
    for (float& gene : m_genes)
        if (shouldMutate(rng))
            gene = std::clamp(gene + noise(rng), -1.0f, 1.0f);
}

}  // namespace wsim

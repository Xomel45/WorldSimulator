#include "sim/Genome.hpp"

#include "sim/Brain.hpp"

#include <algorithm>
#include <cmath>

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
    // Масштаб по числу входов слоя (как в Xavier-инициализации): прежние 0.35 были подобраны под 31 вход/20 скрытых; у сети 55/32 те же
    // веса дали бы вдвое более разбросанные суммы и насыщенные нейроны, поэтому std уменьшен ~ 1/sqrt(fan-in), нормированно к прежнему.
    std::normal_distribution<float> unitDist(0.0f, 1.0f);
    const float stdInHidden  = 0.35f * std::sqrt(31.0f / static_cast<float>(brain::kInputCount));
    const float stdHidOut    = 0.35f * std::sqrt(20.0f / static_cast<float>(brain::kHiddenCount));
    const std::size_t fixedEnd = kTraitGeneCount + static_cast<std::size_t>(brain::kInputHiddenWeights);
    for (std::size_t i = kTraitGeneCount; i < kGeneCount; ++i) {
        float sd = 0.35f;
        if (i < fixedEnd) sd = stdInHidden;                                                                  // input -> hidden
        else if (i >= kTraitGeneCount + kFixedBrainWeightCount && i < kTraitGeneCount + kFixedBrainWeightCount + static_cast<std::size_t>(brain::kHiddenOutputWeights)) sd = stdHidOut;   // hidden -> output
        genes[i] = std::clamp(unitDist(rng) * sd, -1.0f, 1.0f);
    }

    // Желания-выборы (выходы 10..15: охота, труд, стройка, набеги, вождь, исследование) стартуют у новорождённого почти нейтральными
    // (вход ~0.5 -> множитель приоритета ~1): строки их весов hidden->output в 4 раза слабее. Иначе у случайной сети эти группы занятий
    // то глохли бы, то заглушали остальное, и до первых мутаций эволюции было бы нечем отличать мозги. Отбор усиливает их сам.
    const std::size_t plasticStart = kTraitGeneCount + kFixedBrainWeightCount;
    for (int o = 10; o < brain::kOutputCount; ++o)
        for (int h = 0; h < brain::kHiddenCount; ++h) genes[plasticStart + static_cast<std::size_t>(o * brain::kHiddenCount + h)] *= 0.25f;
    // Врождённые инстинкты: скрытые нейроны 24..29 -- "нейроны-инстинкты", каждый заранее подключён к паре-тройке нужных входов и к своему
    // желанию-выбору (охота <- добыча рядом и голод; труд <- день и спокойствие; стройка <- брёвна; набег <- вооружён, смел, сыт; вождь <- далеко от
    // него; исследование <- скука). Это только стартовая точка (обычные гены, их меняют мутации и отбор): новорождённый ведёт себя осмысленно, а не
    // случайно, и эволюции есть от чего оттолкнуться.
    {
        enum In { kHunger = 0, kFear = 2, kSocial = 4, kBoredom = 30, kTribe = 33, kLeaderProx = 36, kNight = 37, kWood = 40, kArmed = 41, kStock = 43, kPrey = 44 };
        const std::size_t plasticStart = kTraitGeneCount + kFixedBrainWeightCount;
        struct Link { int input; float w; };
        struct Instinct { int hidden, output; float bias; std::vector<Link> links; };
        const std::vector<Instinct> instincts = {
            {24, 10, -0.5f, {{kPrey, 1.0f}, {kHunger, 0.8f}, {kArmed, 0.6f}}},
            {25, 11,  0.4f, {{kNight, -1.0f}, {kFear, -0.8f}, {kStock, 0.5f}}},
            {26, 12, -0.4f, {{kWood, 1.0f}, {kTribe, 0.5f}, {kFear, -0.8f}}},
            {27, 13, -0.3f, {{kArmed, 0.8f}, {kTribe, 0.5f}, {kFear, -1.0f}, {kHunger, -0.5f}}},
            {28, 14,  0.1f, {{kTribe, 0.8f}, {kLeaderProx, -0.8f}, {kSocial, -0.5f}}},
            {29, 15,  0.0f, {{kBoredom, 1.0f}, {kHunger, 0.5f}, {kFear, -0.6f}}},
        };
        for (const Instinct& inst : instincts) {
            for (int i = 0; i < brain::kInputCount; ++i) genes[kTraitGeneCount + static_cast<std::size_t>(inst.hidden * brain::kInputCount + i)] = 0.0f;
            for (const Link& l : inst.links) genes[kTraitGeneCount + static_cast<std::size_t>(inst.hidden * brain::kInputCount + l.input)] = l.w;
            genes[kTraitGeneCount + static_cast<std::size_t>(brain::kInputHiddenWeights + inst.hidden)] = inst.bias;
            genes[plasticStart + static_cast<std::size_t>(inst.output * brain::kHiddenCount + inst.hidden)] = 0.5f;
        }
    }
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

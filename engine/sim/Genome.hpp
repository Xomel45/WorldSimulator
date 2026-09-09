#pragma once

#include <cstddef>
#include <random>
#include <span>
#include <vector>

namespace wsim {

// RNG всегда передаётся явно (без глобального состояния) — генетика и мозг
// остаются детерминированными при фиксированном сиде, что нужно для тестов.
using Rng = std::mt19937;

// Плоский вектор генов в [-1, 1]. Первые kTraitGeneCount генов — черты тела
// (см. именованные индексы ниже), остальные kBrainWeightCount — веса мозга
// (см. brain::kWeightCount в Brain.hpp; здесь не включаем Brain.hpp, чтобы
// Genome не тянул за собой Brain — размер завязан константой напрямую).
class Genome {
public:
    static constexpr std::size_t kTraitGeneCount = 5;
    static constexpr std::size_t kBrainWeightCount = 669;  // = brain::kWeightCount
    static constexpr std::size_t kGeneCount = kTraitGeneCount + kBrainWeightCount;

    enum TraitIndex : std::size_t {
        kSpeed = 0,
        kSize,
        kMetabolism,
        kLifespan,
        kFertility,
    };

    Genome() : m_genes(kGeneCount, 0.0f) {}

    [[nodiscard]] float trait(TraitIndex i) const noexcept { return m_genes[static_cast<std::size_t>(i)]; }
    [[nodiscard]] std::span<const float> brainWeights() const noexcept {
        return {m_genes.data() + kTraitGeneCount, kBrainWeightCount};
    }

    // Случайная особь: черты тела — равномерно; веса мозга — со смещением к
    // разумным стартовым инстинктам (не чистый шум), чтобы новорождённая
    // особь не билась в конвульсиях поколениями, пока отбор что-то нащупает.
    [[nodiscard]] static Genome random(Rng& rng);

    // Равномерный покомпонентный кроссовер: каждый ген независимо берётся
    // либо у a, либо у b.
    [[nodiscard]] static Genome crossover(const Genome& a, const Genome& b, Rng& rng);

    // С вероятностью rate на ген — добавляет гауссов шум силы strength,
    // ограничивает результат в [-1, 1].
    void mutate(Rng& rng, float rate, float strength);

private:
    explicit Genome(std::vector<float> genes) : m_genes(std::move(genes)) {}

    std::vector<float> m_genes;
};

}  // namespace wsim

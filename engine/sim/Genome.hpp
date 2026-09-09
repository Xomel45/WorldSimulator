#pragma once

#include <algorithm>
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
    static constexpr std::size_t kTraitGeneCount = 9;
    static constexpr std::size_t kBrainWeightCount = 689;  // = brain::kWeightCount
    static constexpr std::size_t kGeneCount = kTraitGeneCount + kBrainWeightCount;

    // Разбивка весов мозга на фиксированную (input->hidden — "восприятие",
    // наследуется, не меняется в течение жизни) и пластичную (hidden->output
    // — "принятие решений", у Creature есть свой изменяемый рантайм-экземпляр
    // этого куска, см. Creature::learnedWeights) части. Зеркалит
    // brain::kInputHiddenWeights+kHiddenCount / kHiddenOutputWeights+kOutputCount.
    static constexpr std::size_t kFixedBrainWeightCount   = 500;
    static constexpr std::size_t kPlasticBrainWeightCount = 189;

    enum TraitIndex : std::size_t {
        kSpeed = 0,
        kSize,
        kMetabolism,
        kLifespan,
        kFertility,
        kPlasticity,    // скорость обучения в течение жизни — см. Creature::plasticity()

        // Явный темперамент — в отличие от индивидуальности, неявно зашитой в
        // 689 весов мозга, эти черты именованные, инспектируемые и напрямую
        // (детерминированно) модулируют tick(), а не проходят через мозг как
        // ещё один вход, который эволюции ещё предстоит научиться использовать.
        kBoldness,      // храбрость: гасит страх от прямой угрозы/тревоги стаи
        kSociability,   // интроверт/экстраверт: отдача от компании, настороженность к чужакам
        kBondStrength,  // привязанность: насколько опасность близкого ощущается как своя
    };

    Genome() : m_genes(kGeneCount, 0.0f) {}

    [[nodiscard]] float trait(TraitIndex i) const noexcept { return m_genes[static_cast<std::size_t>(i)]; }
    // Прямая установка одной черты (клэмп в [-1,1]) -- симметрична trait().
    // Нужна тестам (форсировать конкретную черту без построения генома вручную)
    // и, в перспективе, god-tools (задать характер существу напрямую).
    void setTrait(TraitIndex i, float value) noexcept {
        m_genes[static_cast<std::size_t>(i)] = std::clamp(value, -1.0f, 1.0f);
    }
    [[nodiscard]] std::span<const float> brainWeights() const noexcept {
        return {m_genes.data() + kTraitGeneCount, kBrainWeightCount};
    }
    [[nodiscard]] std::span<const float> fixedBrainWeights() const noexcept {
        return {m_genes.data() + kTraitGeneCount, kFixedBrainWeightCount};
    }
    // Стартовая точка для Creature::learnedWeights у новорождённой особи —
    // сама генома этот кусок никогда не меняет (обучение не наследуется,
    // см. Creature::tick()/resetLearning()).
    [[nodiscard]] std::span<const float> initialPlasticWeights() const noexcept {
        return {m_genes.data() + kTraitGeneCount + kFixedBrainWeightCount, kPlasticBrainWeightCount};
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

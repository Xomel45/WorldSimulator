#include "sim/Brain.hpp"

#include "test_util.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <random>
#include <span>

using namespace wsim;

namespace {

constexpr int kFixedSize   = brain::kInputHiddenWeights + brain::kHiddenCount;
constexpr int kPlasticSize = brain::kHiddenOutputWeights + brain::kOutputCount;

void checkOutputs(const BrainOutputs& out) {
    WSIM_CHECK(std::isfinite(out.moveX) && out.moveX >= -1.0f && out.moveX <= 1.0f);
    WSIM_CHECK(std::isfinite(out.moveZ) && out.moveZ >= -1.0f && out.moveZ <= 1.0f);
    for (float d : {out.eatDesire, out.fleeDesire, out.mateDesire, out.restDesire, out.defendDesire, out.alarmCall,
                     out.matingCall, out.dominanceDisplay})
        WSIM_CHECK(std::isfinite(d) && d >= 0.0f && d <= 1.0f);
}

void checkPlasticWeightsInRange(std::span<const float> w) {
    for (float x : w)
        WSIM_CHECK(std::isfinite(x) && x >= -2.0f && x <= 2.0f);
}

}  // namespace

int main() {
    static_assert(kFixedSize + kPlasticSize == brain::kWeightCount);

    // Крайние случаи: все веса/входы нулевые, все +1, все -1 -- сеть не должна взрываться в NaN/Inf.
    std::array<float, kFixedSize>   fixedZeros{};
    std::array<float, kPlasticSize> plasticZeros{};
    checkOutputs(brain::evaluate(fixedZeros, plasticZeros, BrainInputs{}).outputs);

    std::array<float, kFixedSize> fixedOnes;
    fixedOnes.fill(1.0f);
    std::array<float, kPlasticSize> plasticOnes;
    plasticOnes.fill(1.0f);
    BrainInputs onesIn{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    checkOutputs(brain::evaluate(fixedOnes, plasticOnes, onesIn).outputs);

    std::array<float, kFixedSize> fixedNegOnes;
    fixedNegOnes.fill(-1.0f);
    std::array<float, kPlasticSize> plasticNegOnes;
    plasticNegOnes.fill(-1.0f);
    BrainInputs negIn{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    checkOutputs(brain::evaluate(fixedNegOnes, plasticNegOnes, negIn).outputs);

    // Случайные веса/входы -- выходы всегда в ожидаемом диапазоне.
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::array<float, kFixedSize> randomFixed{};
    for (float& w : randomFixed) w = dist(rng);
    std::array<float, kPlasticSize> randomPlastic{};
    for (float& w : randomPlastic) w = dist(rng);

    for (int trial = 0; trial < 50; ++trial) {
        BrainInputs in{
            dist(rng), dist(rng), dist(rng), dist(rng), dist(rng), dist(rng), dist(rng),
            dist(rng), dist(rng), dist(rng),
            dist(rng), dist(rng), dist(rng),
            dist(rng), dist(rng), dist(rng),
            dist(rng), dist(rng), dist(rng),
            dist(rng), dist(rng), dist(rng),
            dist(rng),
            dist(rng), dist(rng), dist(rng), dist(rng),
        };
        checkOutputs(brain::evaluate(randomFixed, randomPlastic, in).outputs);
    }

    // reinforce(): положительный reward усиливает связи, которые только что
    // сработали (положительная корреляция hidden/output активаций), отрицательный
    // -- ослабляет. Проверяем на одном явном весе с известными активациями.
    {
        std::array<float, kPlasticSize> weights{};  // все нули
        brain::BrainEvalResult result{};
        result.hiddenActivations.fill(0.0f);
        result.hiddenActivations[0] = 1.0f;   // единственный активный hidden-нейрон
        result.outputActivations.fill(0.0f);
        result.outputActivations[0] = 1.0f;   // единственный активный выход

        brain::reinforce(weights, result, /*reward=*/1.0f, /*learningRate=*/0.1f);
        // w[output=0][hidden=0] = index 0 в hiddenOutput-блоке
        WSIM_CHECK(weights[0] > 0.0f);
        // веса, не связанные с активными hidden/output, не двигаются (bias output[0] тоже двигается)
        WSIM_CHECK(weights[1] == 0.0f);  // output=0,hidden=1 -- hidden неактивен

        const float afterPositive = weights[0];
        brain::reinforce(weights, result, /*reward=*/-1.0f, /*learningRate=*/0.1f);
        WSIM_CHECK(weights[0] < afterPositive);  // отрицательный reward тянет обратно
    }

    // Повторное подкрепление не улетает за клэмп и не даёт NaN/Inf.
    {
        std::array<float, kPlasticSize> weights{};
        brain::BrainEvalResult result{};
        result.hiddenActivations.fill(1.0f);
        result.outputActivations.fill(1.0f);
        for (int i = 0; i < 10000; ++i)
            brain::reinforce(weights, result, /*reward=*/1.0f, /*learningRate=*/0.05f);
        checkPlasticWeightsInRange(weights);
    }

    std::puts("brain_tests: OK");
    return 0;
}

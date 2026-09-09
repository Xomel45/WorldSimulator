#include "sim/Brain.hpp"

#include "test_util.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <random>

using namespace wsim;

namespace {

void checkOutputs(const BrainOutputs& out) {
    WSIM_CHECK(std::isfinite(out.moveX) && out.moveX >= -1.0f && out.moveX <= 1.0f);
    WSIM_CHECK(std::isfinite(out.moveZ) && out.moveZ >= -1.0f && out.moveZ <= 1.0f);
    for (float d : {out.eatDesire, out.fleeDesire, out.mateDesire, out.restDesire, out.alarmCall, out.matingCall})
        WSIM_CHECK(std::isfinite(d) && d >= 0.0f && d <= 1.0f);
}

}  // namespace

int main() {
    // Крайние случаи: все веса/входы нулевые, все +1, все -1 -- сеть не должна взрываться в NaN/Inf.
    std::array<float, brain::kWeightCount> zeros{};
    checkOutputs(brain::evaluate(zeros, BrainInputs{}));

    std::array<float, brain::kWeightCount> ones;
    ones.fill(1.0f);
    BrainInputs onesIn{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    checkOutputs(brain::evaluate(ones, onesIn));

    std::array<float, brain::kWeightCount> negOnes;
    negOnes.fill(-1.0f);
    BrainInputs negIn{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    checkOutputs(brain::evaluate(negOnes, negIn));

    // Случайные веса/входы -- выходы всегда в ожидаемом диапазоне.
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::array<float, brain::kWeightCount> randomWeights{};
    for (float& w : randomWeights) w = dist(rng);

    for (int trial = 0; trial < 50; ++trial) {
        BrainInputs in{
            dist(rng), dist(rng), dist(rng), dist(rng), dist(rng),
            dist(rng), dist(rng), dist(rng),
            dist(rng), dist(rng), dist(rng),
            dist(rng), dist(rng), dist(rng),
            dist(rng), dist(rng),
            dist(rng), dist(rng),
        };
        checkOutputs(brain::evaluate(randomWeights, in));
    }

    std::puts("brain_tests: OK");
    return 0;
}

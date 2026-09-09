#include "sim/Brain.hpp"

#include <array>
#include <cmath>

namespace wsim::brain {

namespace {

float sigmoid(float x) noexcept { return 1.0f / (1.0f + std::exp(-x)); }

std::array<float, kInputCount> toArray(const BrainInputs& in) noexcept {
    return {
        in.hunger, in.energy, in.fear, in.libido, in.social,
        in.foodDirX, in.foodDirZ, in.foodProximity,
        in.threatDirX, in.threatDirZ, in.threatProximity,
        in.mateDirX, in.mateDirZ, in.mateProximity,
        in.crowding, in.noise,
        in.nearbyAlarmLevel, in.nearbyMatingCallLevel,
    };
}

}  // namespace

BrainOutputs evaluate(std::span<const float> weights, const BrainInputs& in) noexcept {
    const auto inputs = toArray(in);

    const std::span<const float> inputHidden  = weights.subspan(0, kInputHiddenWeights);
    const std::span<const float> hiddenBias    = weights.subspan(kInputHiddenWeights, kHiddenCount);
    const std::span<const float> hiddenOutput  = weights.subspan(kInputHiddenWeights + kHiddenCount, kHiddenOutputWeights);
    const std::span<const float> outputBias    = weights.subspan(kInputHiddenWeights + kHiddenCount + kHiddenOutputWeights, kOutputCount);

    std::array<float, kHiddenCount> hidden{};
    for (int h = 0; h < kHiddenCount; ++h) {
        float sum = hiddenBias[static_cast<std::size_t>(h)];
        for (int i = 0; i < kInputCount; ++i)
            sum += inputs[static_cast<std::size_t>(i)] * inputHidden[static_cast<std::size_t>(h * kInputCount + i)];
        hidden[static_cast<std::size_t>(h)] = std::tanh(sum);
    }

    std::array<float, kOutputCount> out{};
    for (int o = 0; o < kOutputCount; ++o) {
        float sum = outputBias[static_cast<std::size_t>(o)];
        for (int h = 0; h < kHiddenCount; ++h)
            sum += hidden[static_cast<std::size_t>(h)] * hiddenOutput[static_cast<std::size_t>(o * kHiddenCount + h)];
        out[static_cast<std::size_t>(o)] = sum;
    }

    return BrainOutputs{
        .moveX      = std::tanh(out[0]),
        .moveZ      = std::tanh(out[1]),
        .eatDesire  = sigmoid(out[2]),
        .fleeDesire = sigmoid(out[3]),
        .mateDesire = sigmoid(out[4]),
        .restDesire = sigmoid(out[5]),
        .alarmCall  = sigmoid(out[6]),
        .matingCall = sigmoid(out[7]),
    };
}

}  // namespace wsim::brain

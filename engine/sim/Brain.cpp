#include "sim/Brain.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace wsim::brain {

namespace {

float sigmoid(float x) noexcept { return 1.0f / (1.0f + std::exp(-x)); }

std::array<float, kInputCount> toArray(const BrainInputs& in) noexcept {
    return {
        in.hunger, in.energy, in.fear, in.libido, in.social, in.respect, in.resentment, in.grief,
        in.foodDirX, in.foodDirZ, in.foodProximity,
        in.threatDirX, in.threatDirZ, in.threatProximity,
        in.mateDirX, in.mateDirZ, in.mateProximity,
        in.familiarCrowding, in.strangerCrowding, in.noise,
        in.nearbyFamiliarAlarmLevel, in.nearbyStrangerAlarmLevel, in.nearbyMatingCallLevel, in.nearbyDominanceLevel,
        in.selfSize,
        in.lovedOneDirX, in.lovedOneDirZ, in.lovedOneProximity, in.lovedOneThreatLevel,
        in.awayFromHome, in.boredom,
        in.waterAhead, in.slopeAhead, in.tribeFlag, in.leaderDirX, in.leaderDirZ, in.leaderProximity, in.night, in.cold, in.rain,
        in.woodFrac, in.armed, in.thirst, in.stockProximity, in.preyProximity, in.intruderProximity, in.squadFlag,
        in.memory[0], in.memory[1], in.memory[2], in.memory[3], in.memory[4], in.memory[5], in.memory[6], in.memory[7],
    };
}

}  // namespace

BrainEvalResult evaluate(std::span<const float> fixedWeights, std::span<const float> plasticWeights,
                          const BrainInputs& in) noexcept {
    const auto inputs = toArray(in);

    // fixedWeights: [inputHidden(1760)][hiddenBias(32)] -- см. Genome::fixedBrainWeights().
    const std::span<const float> inputHidden = fixedWeights.subspan(0, kInputHiddenWeights);
    const std::span<const float> hiddenBias  = fixedWeights.subspan(kInputHiddenWeights, kHiddenCount);
    // plasticWeights: [hiddenOutput(512)][outputBias(16)] -- см. Creature::learnedWeights.
    const std::span<const float> hiddenOutput = plasticWeights.subspan(0, kHiddenOutputWeights);
    const std::span<const float> outputBias   = plasticWeights.subspan(kHiddenOutputWeights, kOutputCount);

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

    BrainEvalResult result{
        .outputs = {
            .moveX        = std::tanh(out[0]),
            .moveZ        = std::tanh(out[1]),
            .eatDesire    = sigmoid(out[2]),
            .fleeDesire   = sigmoid(out[3]),
            .mateDesire   = sigmoid(out[4]),
            .restDesire   = sigmoid(out[5]),
            .defendDesire = sigmoid(out[6]),
            .alarmCall    = sigmoid(out[7]),
            .matingCall   = sigmoid(out[8]),
            .dominanceDisplay = sigmoid(out[9]),
            .huntDesire    = sigmoid(out[10]),
            .workDesire    = sigmoid(out[11]),
            .buildDesire   = sigmoid(out[12]),
            .raidDesire    = sigmoid(out[13]),
            .followDesire  = sigmoid(out[14]),
            .exploreDesire = sigmoid(out[15]),
        },
        .hiddenActivations = hidden,
        .outputActivations = out,
    };
    return result;
}

void reinforce(std::span<float> plasticWeights, const BrainEvalResult& result,
               float reward, float learningRate) noexcept {
    const std::span<float> hiddenOutput = plasticWeights.subspan(0, kHiddenOutputWeights);
    const std::span<float> outputBias   = plasticWeights.subspan(kHiddenOutputWeights, kOutputCount);

    const float delta = reward * learningRate;

    for (int o = 0; o < kOutputCount; ++o) {
        const float postActivation = result.outputActivations[static_cast<std::size_t>(o)];
        for (int h = 0; h < kHiddenCount; ++h) {
            const std::size_t idx = static_cast<std::size_t>(o * kHiddenCount + h);
            float w = hiddenOutput[idx] + delta * result.hiddenActivations[static_cast<std::size_t>(h)] * postActivation;
            hiddenOutput[idx] = std::clamp(w, -2.0f, 2.0f);
        }
        float b = outputBias[static_cast<std::size_t>(o)] + delta * postActivation;
        outputBias[static_cast<std::size_t>(o)] = std::clamp(b, -2.0f, 2.0f);
    }
}

}  // namespace wsim::brain

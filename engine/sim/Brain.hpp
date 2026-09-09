#pragma once

#include <span>

namespace wsim {

// Входы мозга: потребности + локальные ощущения (направление/близость до
// ближайшей еды/угрозы/партнёра — считает будущая мировая система, сюда
// приходят уже готовыми) + скученность соседей + шум (разрывает идеально
// симметричные решения).
struct BrainInputs {
    float hunger, energy, fear, libido, social;
    float foodDirX, foodDirZ, foodProximity;
    float threatDirX, threatDirZ, threatProximity;
    float mateDirX, mateDirZ, mateProximity;
    float crowding, noise;
};

// Выходы: направление движения + "желания" — мир (позже) решает, удаётся ли
// действие (например, есть можно только если рядом реально есть еда).
struct BrainOutputs {
    float moveX, moveZ;
    float eatDesire, fleeDesire, mateDesire, restDesire;
};

namespace brain {

inline constexpr int kInputCount  = 16;
inline constexpr int kHiddenCount = 16;
inline constexpr int kOutputCount = 6;

// Layout: [inputHidden(256)][hiddenBias(16)][hiddenOutput(96)][outputBias(6)]
inline constexpr int kInputHiddenWeights  = kInputCount * kHiddenCount;
inline constexpr int kHiddenOutputWeights = kHiddenCount * kOutputCount;
inline constexpr int kWeightCount =
    kInputHiddenWeights + kHiddenCount + kHiddenOutputWeights + kOutputCount;  // 374

// Прогон сети: tanh на скрытом слое и на moveX/moveZ (нужен знак — направление),
// логистическая сигмоида на "желаниях" (нужен диапазон [0,1]).
[[nodiscard]] BrainOutputs evaluate(std::span<const float> weights, const BrainInputs& in) noexcept;

}  // namespace brain

}  // namespace wsim

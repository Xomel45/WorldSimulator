#pragma once

#include <span>

namespace wsim {

// Входы мозга: потребности + локальные ощущения (направление/близость до
// ближайшей еды/угрозы/партнёра — считает будущая мировая система, сюда
// приходят уже готовыми) + скученность соседей + шум (разрывает идеально
// симметричные решения) + сигналы соседей (тревога/брачный зов — реальная
// коммуникация между существами, не просто приватная метрика).
struct BrainInputs {
    float hunger, energy, fear, libido, social;
    float foodDirX, foodDirZ, foodProximity;
    float threatDirX, threatDirZ, threatProximity;
    float mateDirX, mateDirZ, mateProximity;
    float crowding, noise;
    float nearbyAlarmLevel, nearbyMatingCallLevel;
};

// Выходы: направление движения + "желания" + сигналы наружу (крик тревоги,
// брачный зов — слышат соседние существа через их nearbyAlarmLevel/
// nearbyMatingCallLevel). Мир (позже) решает, удаётся ли действие (например,
// есть можно только если рядом реально есть еда).
struct BrainOutputs {
    float moveX, moveZ;
    float eatDesire, fleeDesire, mateDesire, restDesire;
    float alarmCall, matingCall;
};

namespace brain {

inline constexpr int kInputCount  = 18;
inline constexpr int kHiddenCount = 20;
inline constexpr int kOutputCount = 8;

// Layout: [inputHidden(360)][hiddenBias(20)][hiddenOutput(160)][outputBias(8)]
inline constexpr int kInputHiddenWeights  = kInputCount * kHiddenCount;
inline constexpr int kHiddenOutputWeights = kHiddenCount * kOutputCount;
inline constexpr int kWeightCount =
    kInputHiddenWeights + kHiddenCount + kHiddenOutputWeights + kOutputCount;  // 548

// Прогон сети: tanh на скрытом слое и на moveX/moveZ (нужен знак — направление),
// логистическая сигмоида на "желаниях" и сигналах (нужен диапазон [0,1]).
[[nodiscard]] BrainOutputs evaluate(std::span<const float> weights, const BrainInputs& in) noexcept;

}  // namespace brain

}  // namespace wsim

#pragma once

#include <span>

namespace wsim {

// Входы мозга: потребности + локальные ощущения (направление/близость до
// ближайшей еды/угрозы/партнёра — считает будущая мировая система, сюда
// приходят уже готовыми) + скученность соседей + шум (разрывает идеально
// симметричные решения) + сигналы соседей (тревога/брачный зов — реальная
// коммуникация между существами, не просто приватная метрика) + собственный
// размер (самооценка себя для выбора "драться или бежать" — без него решение
// драться было бы не про силы, а про угадывание).
struct BrainInputs {
    float hunger, energy, fear, libido, social;
    float foodDirX, foodDirZ, foodProximity;
    float threatDirX, threatDirZ, threatProximity;
    float mateDirX, mateDirZ, mateProximity;
    float crowding, noise;
    float nearbyAlarmLevel, nearbyMatingCallLevel;
    float selfSize;
};

// Выходы: направление движения + "желания" + сигналы наружу (крик тревоги,
// брачный зов — слышат соседние существа через их nearbyAlarmLevel/
// nearbyMatingCallLevel) + defendDesire (fight, а не только flee). Мир
// (позже) решает, удаётся ли действие (например, есть можно только если
// рядом реально есть еда, а драка — только если противник действительно рядом).
struct BrainOutputs {
    float moveX, moveZ;
    float eatDesire, fleeDesire, mateDesire, restDesire, defendDesire;
    float alarmCall, matingCall;
};

namespace brain {

inline constexpr int kInputCount  = 19;
inline constexpr int kHiddenCount = 20;
inline constexpr int kOutputCount = 9;

// Layout: [inputHidden(380)][hiddenBias(20)][hiddenOutput(180)][outputBias(9)]
inline constexpr int kInputHiddenWeights  = kInputCount * kHiddenCount;
inline constexpr int kHiddenOutputWeights = kHiddenCount * kOutputCount;
inline constexpr int kWeightCount =
    kInputHiddenWeights + kHiddenCount + kHiddenOutputWeights + kOutputCount;  // 589

// Прогон сети: tanh на скрытом слое и на moveX/moveZ (нужен знак — направление),
// логистическая сигмоида на "желаниях" и сигналах (нужен диапазон [0,1]).
[[nodiscard]] BrainOutputs evaluate(std::span<const float> weights, const BrainInputs& in) noexcept;

}  // namespace brain

}  // namespace wsim

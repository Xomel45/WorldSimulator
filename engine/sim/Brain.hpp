#pragma once

#include <array>
#include <span>

namespace wsim {

// Входы мозга: потребности (включая respect — желание социального статуса,
// см. Needs::respect/Creature::earnRespect(); resentment — обида/зависть,
// накапливается у завистливых особей рядом с чужой демонстрацией статуса,
// см. Genome::kEnvy/Creature::envy(); grief — горе по утрате близкого, см.
// Creature::bondStrength()) + локальные ощущения (направление/близость до
// ближайшей еды/угрозы/партнёра — считает будущая мировая система, сюда
// приходят уже готовыми) + скученность своих/чужих (familiarCrowding —
// известные существа: партнёр/родители/дети; strangerCrowding —
// незнакомцы) + шум (разрывает идеально симметричные решения) + сигналы
// соседей (тревога отдельно от своих/чужих — см. Genome::kEmpathy; брачный
// зов; демонстрация статуса — реальная коммуникация между существами, не
// просто приватная метрика) + собственный размер (самооценка себя для
// выбора "драться или бежать" — без него решение драться было бы не про
// силы, а про угадывание) + ближайший из семьи (партнёр/родители/дети —
// direction/proximity/threatLevel, см. Creature::bondedPartnerId/
// parentIds/childIds) — то, что делает "любовь" поведением, а не просто
// словом + самостоятельно вычисляемые сигналы (awayFromHome —
// Creature::awayFromHome(); boredom — Creature::boredomLevel()).
struct BrainInputs {
    float hunger, energy, fear, libido, social, respect, resentment, grief;
    float foodDirX, foodDirZ, foodProximity;
    float threatDirX, threatDirZ, threatProximity;
    float mateDirX, mateDirZ, mateProximity;
    float familiarCrowding, strangerCrowding, noise;
    float nearbyFamiliarAlarmLevel, nearbyStrangerAlarmLevel, nearbyMatingCallLevel, nearbyDominanceLevel;
    float selfSize;
    float lovedOneDirX, lovedOneDirZ, lovedOneProximity, lovedOneThreatLevel;
    float awayFromHome, boredom;
};

// Выходы: направление движения + "желания" + сигналы наружу (крик тревоги,
// брачный зов, демонстрация статуса — слышат соседние существа через их
// nearbyFamiliarAlarmLevel/nearbyStrangerAlarmLevel/nearbyMatingCallLevel/
// nearbyDominanceLevel) +
// defendDesire (fight, а не только flee). dominanceDisplay одновременно и
// "желание быть уважаемым" этого существа, и сигнал наружу для соседей (та
// же двойная роль, что у alarmCall/matingCall) — то, что делает статус
// поведением, а не приватным числом. Мир (позже) решает, удаётся ли
// действие (например, есть можно только если рядом реально есть еда, а
// демонстрация статуса выигрывает контест только если оспаривающих не
// нашлось сильнее — см. Creature::earnRespect()).
struct BrainOutputs {
    float moveX, moveZ;
    float eatDesire, fleeDesire, mateDesire, restDesire, defendDesire;
    float alarmCall, matingCall, dominanceDisplay;
};

namespace brain {

inline constexpr int kInputCount  = 31;
inline constexpr int kHiddenCount = 20;
inline constexpr int kOutputCount = 10;

// Layout веса делятся на фиксированную (input->hidden + hiddenBias — из
// Genome::fixedBrainWeights(), наследуется, не меняется в течение жизни) и
// пластичную (hiddenOutput + outputBias — из Creature::learnedWeights,
// стартует от Genome::initialPlasticWeights(), но дальше живёт своей жизнью
// и меняется через reinforce(); обучение НЕ наследуется — Дарвин, не
// Ламарк) части. См. Genome::kFixedBrainWeightCount/kPlasticBrainWeightCount.
inline constexpr int kInputHiddenWeights  = kInputCount * kHiddenCount;
inline constexpr int kHiddenOutputWeights = kHiddenCount * kOutputCount;
inline constexpr int kWeightCount =
    kInputHiddenWeights + kHiddenCount + kHiddenOutputWeights + kOutputCount;  // 850

// Промежуточные активации нужны наружу для reinforce() (Hebbian-обновление
// требует "кто с кем вместе сработал"). outputActivations дублирует значения
// из outputs, но в индексируемом виде — удобнее для цикла обновления весов,
// чем разбор именованных полей структуры.
struct BrainEvalResult {
    BrainOutputs outputs;
    std::array<float, kHiddenCount> hiddenActivations;
    std::array<float, kOutputCount> outputActivations;
};

// Прогон сети: tanh на скрытом слое и на moveX/moveZ (нужен знак — направление),
// логистическая сигмоида на "желаниях" и сигналах (нужен диапазон [0,1]).
[[nodiscard]] BrainEvalResult evaluate(
    std::span<const float> fixedWeights, std::span<const float> plasticWeights,
    const BrainInputs& in) noexcept;

// Reward-modulated Hebbian: Δw = learningRate * reward * hidden[h] * outputActivation[o]
// (и симметрично для outputBias, без множителя на hidden). Обновляет
// plasticWeights на месте, ограничивает результат [-2, 2] (шире генного
// [-1, 1] — это рантайм-состояние, не ген, обучению нужен простор).
void reinforce(std::span<float> plasticWeights, const BrainEvalResult& result,
                float reward, float learningRate) noexcept;

}  // namespace brain

}  // namespace wsim

#pragma once

#include <cstdint>

namespace wsim {

// Push-константы для официальных шейдеров FSR 1.0 (ffx_fsr1.h).
// Считаются на CPU функциями FsrEasuCon/FsrRcasCon из того же хедера.
struct FsrConstants {
    uint32_t easu[4][4];  // con0..con3 для EASU
    uint32_t rcas[4];     // con0 для RCAS
};

// renderW/H — разрешение рендера, outputW/H — разрешение вывода.
// sharpnessStops: 0.0 = максимальная резкость, 2.0 = слабая (шкала AMD в "стопах").
[[nodiscard]] FsrConstants computeFsrConstants(
    uint32_t renderW, uint32_t renderH,
    uint32_t outputW, uint32_t outputH,
    float sharpnessStops);

}  // namespace wsim

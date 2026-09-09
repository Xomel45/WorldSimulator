#include "renderer/FsrCon.hpp"

// Официальные хедеры AMD FidelityFX FSR 1.0 (GPUOpen, MIT).
// A_CPU: включает CPU-варианты функций подготовки констант.
// Изолированы в этом TU — ffx_a.h определяет глобальные макросы (AU1, AF1, ...)
// и ожидает C-математику (fabsf, cosf, ...) в глобальном неймспейсе.
#include <math.h>   // NOLINT: требование ffx_a.h

#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

namespace wsim {

FsrConstants computeFsrConstants(
    uint32_t renderW, uint32_t renderH,
    uint32_t outputW, uint32_t outputH,
    float sharpnessStops
) {
    FsrConstants c{};
    FsrEasuCon(c.easu[0], c.easu[1], c.easu[2], c.easu[3],
               (AF1)renderW, (AF1)renderH,   // рендер-размер
               (AF1)renderW, (AF1)renderH,   // размер входной текстуры
               (AF1)outputW, (AF1)outputH);  // размер вывода
    FsrRcasCon(c.rcas, sharpnessStops);
    return c;
}

}  // namespace wsim

#include "sim/Terrain.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>

using namespace wsim;

namespace {
constexpr float kHalfSize = 100.0f;
constexpr int   kGridStep = 5;  // сэмплируем каждые 5 единиц по X/Z
}  // namespace

int main() {
    // Детерминизм: тот же сид -> та же высота везде.
    Terrain a(kHalfSize, /*seed=*/42);
    Terrain b(kHalfSize, /*seed=*/42);
    for (float x = -kHalfSize; x <= kHalfSize; x += kGridStep)
        for (float z = -kHalfSize; z <= kHalfSize; z += kGridStep)
            WSIM_CHECK(a.heightAt(x, z) == b.heightAt(x, z));

    // Разные сиды -> разный рельеф (хотя бы где-то, не обязательно везде).
    Terrain c(kHalfSize, /*seed=*/1337);
    bool anyDifferent = false;
    for (float x = -kHalfSize; x <= kHalfSize; x += kGridStep)
        for (float z = -kHalfSize; z <= kHalfSize; z += kGridStep)
            if (a.heightAt(x, z) != c.heightAt(x, z)) anyDifferent = true;
    WSIM_CHECK(anyDifferent);

    // Высота конечна везде в границах мира (ловит NaN/Inf из шумовой математики).
    int landCount = 0, waterCount = 0;
    for (float x = -kHalfSize; x <= kHalfSize; x += kGridStep) {
        for (float z = -kHalfSize; z <= kHalfSize; z += kGridStep) {
            const float h = a.heightAt(x, z);
            WSIM_CHECK(std::isfinite(h));
            WSIM_CHECK(a.isUnderwater(x, z) == (h < Terrain::kSeaLevel));
            if (h < Terrain::kSeaLevel) ++waterCount;
            else ++landCount;
        }
    }

    // Смещение должно давать в основном сушу, но не полное отсутствие воды
    // (иначе "реки/озёра/океаны" в принципе негде появиться) -- грубая
    // проверка баланса констант, не точное соотношение.
    WSIM_CHECK(landCount > waterCount);
    WSIM_CHECK(waterCount > 0);

    std::puts("terrain_tests: OK");
    return 0;
}

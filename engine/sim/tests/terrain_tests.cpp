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

    // Правки игрока: raise поднимает в центре, не трогает за радиусом, не ломает основу.
    {
        Terrain t(kHalfSize, 7);
        WSIM_CHECK(!t.edited());
        const float before = t.heightAt(10.0f, 10.0f), faraway = t.heightAt(60.0f, 60.0f);
        const auto  rect   = t.raise(10.0f, 10.0f, 8.0f, 5.0f);
        WSIM_CHECK(t.edited());
        WSIM_CHECK(rect.x0 < 10.0f && rect.x1 > 10.0f && rect.z0 < 10.0f && rect.z1 > 10.0f);
        WSIM_CHECK(std::fabs(t.heightAt(10.0f, 10.0f) - (before + 5.0f)) < 0.05f);
        WSIM_CHECK(t.heightAt(60.0f, 60.0f) == faraway);
        WSIM_CHECK(t.baseHeightAt(10.0f, 10.0f) == before);
        // Плавность: между вершинами правок нет скачков.
        WSIM_CHECK(std::fabs(t.heightAt(10.0f, 10.0f) - t.heightAt(10.2f, 10.0f)) < 0.5f);

        // flatten подтягивает к цели: многократно -- сходится.
        for (int i = 0; i < 200; ++i) t.flatten(10.0f, 10.0f, 8.0f, 2.0f, 0.2f);
        WSIM_CHECK(std::fabs(t.heightAt(10.0f, 10.0f) - 2.0f) < 0.1f);

        // Правки в отрицательных координатах (плитки с отрицательным индексом).
        Terrain n(kHalfSize, 7);
        const float nb = n.heightAt(-40.0f, -70.0f);
        n.raise(-40.0f, -70.0f, 6.0f, -3.0f);
        WSIM_CHECK(std::fabs(n.heightAt(-40.0f, -70.0f) - (nb - 3.0f)) < 0.05f);

        // Лимит суммарной поправки.
        for (int i = 0; i < 100; ++i) n.raise(-40.0f, -70.0f, 6.0f, -50.0f);
        WSIM_CHECK(n.heightAt(-40.0f, -70.0f) - n.baseHeightAt(-40.0f, -70.0f) >= -Terrain::kMaxEdit - 0.01f);
    }

    // Климат: чистая функция сида (детерминирована, в 0..1, не зависит от правок рельефа), высота остужает,
    // на больших расстояниях встречаются все биомы, имена заданы.
    {
        Terrain c1(kHalfSize, 21), c2(kHalfSize, 21);
        for (float x = -500; x <= 500; x += 70)
            for (float z = -500; z <= 500; z += 70) {
                const Climate p = c1.climateAt(x, z), q = c2.climateAt(x, z);
                WSIM_CHECK(p.temperature == q.temperature && p.moisture == q.moisture);
                WSIM_CHECK(p.temperature >= 0.0f && p.temperature <= 1.0f && p.moisture >= 0.0f && p.moisture <= 1.0f);
            }
        const Climate before = c1.climateAt(30.0f, 30.0f);
        c1.raise(30.0f, 30.0f, 10.0f, 20.0f);
        WSIM_CHECK(c1.climateAt(30.0f, 30.0f).temperature == before.temperature);  // правки рельефа климат не трогают

        const Climate warm{0.6f, 0.5f};
        WSIM_CHECK(withAltitude(warm, 2.0f).temperature == warm.temperature);        // ниже порога -- без поправки
        WSIM_CHECK(withAltitude(warm, 10.0f).temperature < warm.temperature - 0.2f);
        WSIM_CHECK(withAltitude(warm, 500.0f).temperature == 0.0f);                  // не уходит в минус

        WSIM_CHECK(classify({0.1f, 0.1f}) == Biome::Tundra && classify({0.1f, 0.8f}) == Biome::Taiga);
        WSIM_CHECK(classify({0.5f, 0.2f}) == Biome::Plains && classify({0.5f, 0.8f}) == Biome::Forest);
        WSIM_CHECK(classify({0.9f, 0.1f}) == Biome::Desert && classify({0.9f, 0.5f}) == Biome::Savanna &&
                   classify({0.9f, 0.9f}) == Biome::Jungle);

        int cnt[kBiomeCount] = {}, total = 0;
        for (unsigned seed : {5u, 11u, 29u}) {
            Terrain t(kHalfSize, seed);
            for (float x = -2000; x <= 2000; x += 40)
                for (float z = -2000; z <= 2000; z += 40) { ++cnt[static_cast<int>(t.biomeAt(x, z))]; ++total; }
        }
        for (int b = 0; b < kBiomeCount; ++b) {
            WSIM_CHECK(cnt[b] > total / 25);  // каждый биом занимает заметную долю (не менее 4%) ...
            WSIM_CHECK(cnt[b] < total / 2);   // ... и ни один не съел карту
            WSIM_CHECK(biomeName(static_cast<Biome>(b))[0] != '?');
        }
    }

    std::puts("terrain_tests: OK");
    return 0;
}

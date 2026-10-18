#include "sim/Trees.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

using namespace wsim;

namespace {
bool sameTree(const TreeInstance& a, const TreeInstance& b) {
    return a.x == b.x && a.z == b.z && a.scale == b.scale && a.yaw == b.yaw && a.species == b.species && a.variant == b.variant;
}
}  // namespace

int main() {
    Terrain terrain(100.0f, 5);

    // Детерминизм: те же деревья при повторном вызове; другой сид -- другой лес.
    const auto a = Trees::inRect(terrain, -100, -100, 100, 100);
    const auto b = Trees::inRect(terrain, -100, -100, 100, 100);
    WSIM_CHECK(!a.empty() && a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) WSIM_CHECK(sameTree(a[i], b[i]));
    {
        Terrain other(100.0f, 99);
        const auto c = Trees::inRect(other, -100, -100, 100, 100);
        WSIM_CHECK(c.size() != a.size() || !sameTree(a[0], c[0]));
    }

    // Разбиение на прямоугольники ничего не теряет и не дублирует (так мир делится на чанки).
    {
        auto key = [](const TreeInstance& t) { return std::make_pair(t.x, t.z); };
        std::set<std::pair<float, float>> whole, parts;
        for (const auto& t : Trees::inRect(terrain, -64, -64, 64, 64)) whole.insert(key(t));
        for (float x : {-64.0f, 0.0f})
            for (float z : {-64.0f, 0.0f})
                for (const auto& t : Trees::inRect(terrain, x, z, x + 64, z + 64)) WSIM_CHECK(parts.insert(key(t)).second);  // дублей нет
        WSIM_CHECK(whole == parts);
    }

    // Все деревья на годной земле; не слишком тесно; оба вида встречаются; есть и рощи, и поляны.
    {
        std::set<TreeSpecies> kinds;
        for (const auto& t : a) {
            const float h = terrain.heightAt(t.x, t.z);
            WSIM_CHECK(h >= Trees::kMinHeight && h <= (t.species == TreeSpecies::Pine ? Trees::kMaxHeightPine : Trees::kMaxHeight));
            WSIM_CHECK(!terrain.isUnderwater(t.x, t.z));
            WSIM_CHECK(t.scale >= 0.6f && t.scale <= 1.7f && t.variant < kTreeVariants);
            kinds.insert(t.species);
        }
        WSIM_CHECK(kinds.size() >= 2);  // участок 200x200 захватывает несколько биомов
        double minDist = 1e9;
        for (std::size_t i = 0; i < a.size(); ++i)
            for (std::size_t j = i + 1; j < a.size(); ++j) minDist = std::min(minDist, static_cast<double>(std::hypot(a[i].x - a[j].x, a[i].z - a[j].z)));
        WSIM_CHECK(minDist > 1.5);  // ячейка 7 с отступом от краёв -- вплотную не бывает

        // Рощи/поляны: по квадратам 28x28 густота заметно разная.
        int emptySquares = 0, denseSquares = 0;
        for (float x = -84; x < 84; x += 28)
            for (float z = -84; z < 84; z += 28) {
                const auto n = Trees::inRect(terrain, x, z, x + 28, z + 28).size();
                emptySquares += n == 0;
                denseSquares += n >= 4;
            }
        WSIM_CHECK(denseSquares > 0);
        WSIM_CHECK(emptySquares + denseSquares > 3);
    }

    // Блокировка (дом/куст): деревья в запрещённой зоне не растут.
    {
        const TreeInstance first = a.front();
        const Trees::Blocker blocker = [&](float x, float z, float r) { return std::hypot(x - first.x, z - first.z) < r + 5.0f; };
        const auto filtered = Trees::inRect(terrain, -100, -100, 100, 100, &blocker);
        WSIM_CHECK(filtered.size() < a.size());
        for (const auto& t : filtered) WSIM_CHECK(!blocker(t.x, t.z, 0.0f));
    }

    // Столкновение со стволом: в корне -- да, в 3 юнитах от любого дерева -- нет.
    {
        for (std::size_t i = 0; i < std::min<std::size_t>(a.size(), 25); ++i) WSIM_CHECK(Trees::blocksPoint(terrain, a[i].x, a[i].z));
        int free = 0;
        for (float x = -90; x < 90; x += 9)
            for (float z = -90; z < 90; z += 9) {
                bool near = false;
                for (const auto& t : a) if (std::hypot(t.x - x, t.z - z) < 3.0) { near = true; break; }
                if (!near) { WSIM_CHECK(!Trees::blocksPoint(terrain, x, z)); ++free; }
            }
        WSIM_CHECK(free > 10);
    }

    // Правки игрока меняют лес: опустили землю под деревом ниже берега -- дерева нет; подняли гору выше снега -- тоже.
    {
        Terrain t2(100.0f, 5);
        const auto before = Trees::inRect(t2, -100, -100, 100, 100);
        const TreeInstance victim = before[before.size() / 2];
        for (int i = 0; i < 40; ++i) t2.raise(victim.x, victim.z, 6.0f, -1.5f);  // яма -- уйдёт под воду
        const auto after = Trees::inRect(t2, -100, -100, 100, 100);
        bool present = false;
        for (const auto& t : after) present |= (t.x == victim.x && t.z == victim.z);
        WSIM_CHECK(!present);
        WSIM_CHECK(after.size() < before.size());
        WSIM_CHECK(!Trees::blocksPoint(t2, victim.x, victim.z));  // и ствола больше нет
    }

    // Биомы определяют лес: в каждом -- свой набор видов и своя густота. Проверяем только "внутренности" биомов
    // (граница дрожит на ячейку) и низину до порога остывания, чтобы высота не меняла климат.
    {
        const auto allowed = [](Biome b, TreeSpecies s) {
            switch (b) {
                case Biome::Forest:
                case Biome::Plains:  return s == TreeSpecies::Oak || s == TreeSpecies::Birch;
                case Biome::Taiga:   return s == TreeSpecies::Pine || s == TreeSpecies::Birch;
                case Biome::Tundra:  return s == TreeSpecies::Pine;
                case Biome::Savanna: return s == TreeSpecies::Acacia;
                case Biome::Desert:  return s == TreeSpecies::Cactus;
                case Biome::Jungle:  return s == TreeSpecies::Oak || s == TreeSpecies::Palm;
            }
            return false;
        };
        // Точка внутри биома: климат в ней и со сдвигом на 0.06 по обеим осям -- один и тот же биом.
        Terrain t3(100.0f, 11);
        const auto interior = [&](float x, float z, Biome& out) {
            const Climate c = t3.climateAt(x, z);
            out = classify(c);
            for (float dt : {-0.06f, 0.06f})
                for (float dm : {-0.06f, 0.06f})
                    if (classify({c.temperature + dt, c.moisture + dm}) != out) return false;
            return true;
        };
        double trees[kBiomeCount] = {}, area[kBiomeCount] = {};
        for (unsigned seed : {5u, 11u, 29u, 77u, 1234u, 9999u}) {
          t3 = Terrain(100.0f, seed);
          for (float cx = -2400; cx < 2400; cx += 60)
            for (float cz = -2400; cz < 2400; cz += 60) {
                Biome b{};
                if (!interior(cx + 30, cz + 30, b)) continue;
                int land = 0;  // для густоты берём участки, почти целиком лежащие на суше (вода и пляж деревьев не дают)
                for (float dx : {0.0f, 30.0f, 60.0f})
                    for (float dz : {0.0f, 30.0f, 60.0f}) land += t3.heightAt(cx + dx, cz + dz) > Trees::kMinHeight + 0.3f;
                if (land < 9) continue;
                area[static_cast<int>(b)] += 60.0 * 60.0;
                for (const auto& t : Trees::inRect(t3, cx, cz, cx + 60, cz + 60)) {
                    trees[static_cast<int>(b)] += 1.0;
                    Biome tb{};
                    // на высоте климат остывает (деревья мельчают до хвойных) -- виды проверяем только в низине
                    if (t3.heightAt(t.x, t.z) <= kLapseStart && interior(t.x, t.z, tb)) WSIM_CHECK(allowed(tb, t.species));
                }
            }
        }
        double density[kBiomeCount] = {};
        for (int b = 0; b < kBiomeCount; ++b) {
            WSIM_CHECK(area[b] > 0.0);  // каждый биом нашёлся
            density[b] = trees[b] / area[b];
        }
        const auto D = [&](Biome b) { return density[static_cast<int>(b)]; };
        WSIM_CHECK(D(Biome::Forest) > 3.0 * D(Biome::Desert));
        WSIM_CHECK(D(Biome::Jungle) > 1.8 * D(Biome::Tundra));
        WSIM_CHECK(D(Biome::Forest) > 1.4 * D(Biome::Plains));
        WSIM_CHECK(D(Biome::Forest) > 1.4 * D(Biome::Savanna));
        WSIM_CHECK(D(Biome::Taiga) > 1.2 * D(Biome::Tundra));
    }

    std::puts("trees_tests: OK");
    return 0;
}

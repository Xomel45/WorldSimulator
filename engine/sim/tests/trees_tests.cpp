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
        int oaks = 0, birches = 0;
        for (const auto& t : a) {
            const float h = terrain.heightAt(t.x, t.z);
            WSIM_CHECK(h >= Trees::kMinHeight && h <= Trees::kMaxHeight);
            WSIM_CHECK(!terrain.isUnderwater(t.x, t.z));
            WSIM_CHECK(t.scale >= 0.8f && t.scale <= 1.3f + 1e-4f && t.variant < kTreeVariants);
            (t.species == TreeSpecies::Oak ? oaks : birches)++;
        }
        WSIM_CHECK(oaks > 0 && birches > 0);
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

    std::puts("trees_tests: OK");
    return 0;
}

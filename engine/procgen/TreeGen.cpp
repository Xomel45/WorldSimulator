#include "procgen/TreeGen.hpp"

#include <cmath>
#include <vector>

namespace wsim::procgen {

namespace {

// Детерминированные "случайные" числа по (вид, вариант, номер).
float rnd(int species, int variant, int k) {
    const float h = std::sin(static_cast<float>(species * 101 + variant * 37 + k * 13) * 12.9898f + 4.1414f) * 43758.5453f;
    return h - std::floor(h);
}
float rrange(int species, int variant, int k, float a, float b) { return a + (b - a) * rnd(species, variant, k); }

// addBlob с учётом тела кроны: запоминает эллипсоид и диапазон своих треугольников, чтобы потом убрать то,
// что целиком спрятано внутри соседних комков (removeBuriedTriangles). Шероховатость уменьшает радиус до
// (1 - 0.5 * roughness), а грани -- вписанные в сферу, поэтому проверка идёт по заведомо сплошной части.
void blobT(MeshData& m, std::vector<SolidEllipsoid>& solids, glm::vec3 center, glm::vec3 radii, glm::vec3 color, float roughness,
           std::uint32_t seed, int levels, float material) {
    const std::size_t first = m.indices.size() / 3;
    addBlob(m, center, radii, color, roughness, seed, levels, material);
    // Ещё запас на гранёность: вписанная сфера икосаэдра ~0.79 описанной (levels 0), у levels 1 ~0.94.
    const float facets = levels == 0 ? 0.78f : 0.92f;
    solids.push_back({center, radii * ((1.0f - 0.5f * roughness) * facets), first, m.indices.size() / 3});
}

// Ствол одной трубой через точки оси: кольцо на каждой точке общее для соседних секций (ориентация из
// средней касательной, без поворота между секциями) -- на стыках нет ступенек. colors[i] -- цвет секции i.
void addTube(MeshData& m, const std::vector<glm::vec3>& pts, const std::vector<float>& radii, const std::vector<glm::vec3>& colors,
             int sides) {
    const std::size_t n = pts.size();
    std::vector<std::vector<glm::vec3>> rings(n);
    for (std::size_t i = 0; i < n; ++i) {
        const glm::vec3 a = pts[i > 0 ? i - 1 : i], b = pts[i + 1 < n ? i + 1 : i];
        const glm::vec3 tan = glm::normalize(b - a);
        const glm::vec3 u = glm::normalize(glm::cross(tan, glm::vec3(0, 0, 1))), w = glm::cross(tan, u);
        for (int k = 0; k < sides; ++k) {
            const float ang = 6.2831853f * static_cast<float>(k) / static_cast<float>(sides);
            rings[i].push_back(pts[i] + (u * std::cos(ang) + w * std::sin(ang)) * radii[i]);
        }
    }
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const glm::vec3 mid = (pts[i] + pts[i + 1]) * 0.5f;
        for (int k = 0; k < sides; ++k) {
            const std::size_t k1 = static_cast<std::size_t>((k + 1) % sides), k0 = static_cast<std::size_t>(k);
            addQuad(m, rings[i][k0], rings[i][k1], rings[i + 1][k1], rings[i + 1][k0], mid, colors[i], kPlainMaterial);
        }
    }
}

MeshData oak(int v) {
    MeshData m;
    std::vector<SolidEllipsoid> solids;
    const glm::vec3 bark{0.30f, 0.21f, 0.13f};
    const float height = rrange(0, v, 1, 1.8f, 2.3f), r0 = rrange(0, v, 2, 0.36f, 0.46f);
    const float lean = rrange(0, v, 3, -0.18f, 0.18f);
    const glm::vec3 top{lean, height, -lean * 0.5f};

    // Ствол: корень уходит под землю; чуть сужается; снизу тёмнее (мох/земля).
    addTube(m, {{0, -0.5f, 0}, {lean * 0.3f, height * 0.45f, 0}, top}, {r0 * 1.15f, r0 * 0.9f, r0 * 0.62f}, {bark * 0.85f, bark}, 7);

    // Две толстые ветви в стороны под крону.
    for (int k = 0; k < 2; ++k) {
        const float a = rrange(0, v, 10 + k, 0.0f, 6.28f) + static_cast<float>(k) * 3.14f;
        const glm::vec3 from = top + glm::vec3(0, -0.55f, 0);
        const glm::vec3 to   = from + glm::vec3(std::cos(a) * 1.05f, 0.95f, std::sin(a) * 1.05f);
        addTaperedPrism(m, from, to, r0 * 0.42f, r0 * 0.2f, 5, bark);
    }

    // Крона: широкая, из комков разной величины; цвета -- оттенки тёмной дубовой зелени. Материал "пятнистый".
    const glm::vec3 leaf{0.18f, 0.37f, 0.11f};
    const float cy = height + 1.05f;
    blobT(m, solids, {top.x, cy + 0.3f, top.z}, {2.2f, 1.7f, 2.2f}, leaf, 0.55f, 1 + v, 1, kMottled);
    for (int k = 0; k < 3; ++k) {
        const float a = rrange(0, v, 20 + k, 0.0f, 6.28f), d = rrange(0, v, 23 + k, 1.2f, 1.7f);
        blobT(m, solids, {top.x + std::cos(a) * d, cy - 0.15f + 0.3f * rnd(0, v, 26 + k), top.z + std::sin(a) * d},
                {1.45f, 1.15f, 1.45f}, leaf * (0.9f + 0.2f * rnd(0, v, 30 + k)), 0.5f, 10 + 3 * k + v, 1, kMottled);
    }
    blobT(m, solids, {top.x + rrange(0, v, 40, -0.5f, 0.5f), cy + 1.3f, top.z + rrange(0, v, 41, -0.5f, 0.5f)}, {1.2f, 0.85f, 1.2f},
            leaf * 1.12f, 0.5f, 50 + v, 0, kMottled);
    removeBuriedTriangles(m, solids, 1.0f);
    return m;
}

MeshData birch(int v) {
    MeshData m;
    std::vector<SolidEllipsoid> solids;
    const glm::vec3 white{0.88f, 0.87f, 0.82f}, dark{0.16f, 0.15f, 0.14f};
    const float height = rrange(1, v, 1, 5.2f, 6.4f), r0 = rrange(1, v, 2, 0.13f, 0.17f);
    const float leanX = rrange(1, v, 3, -0.35f, 0.35f), leanZ = rrange(1, v, 4, -0.35f, 0.35f);
    auto at = [&](float t) { return glm::vec3(leanX * t * t, -0.4f + (height + 0.4f) * t, leanZ * t * t); };  // слегка изогнутая ось

    // Ствол из секций: белый с редкими тёмными пояском-пятнами (характерная кора).
    // Кольца общие для соседних секций (одна система координат из касательной оси, без поворота
    // между секциями) -- иначе на стыках получаются ступеньки и торчащие углы.
    const int segs = 7, sides = 6;
    auto ring = [&](float t, float radius, int k) {
        const glm::vec3 tan = glm::normalize(at(std::min(t + 0.02f, 1.0f)) - at(std::max(t - 0.02f, 0.0f)));
        const glm::vec3 u   = glm::normalize(glm::cross(tan, glm::vec3(0, 0, 1))), w = glm::cross(tan, u);
        const float     a   = 6.2831853f * static_cast<float>(k) / static_cast<float>(sides);
        return at(t) + (u * std::cos(a) + w * std::sin(a)) * radius;
    };
    for (int s = 0; s < segs; ++s) {
        const float t0 = static_cast<float>(s) / static_cast<float>(segs), t1 = static_cast<float>(s + 1) / static_cast<float>(segs);
        const bool band = rnd(1, v, 60 + s) > 0.55f && s > 0;
        const float rA = r0 * (1.0f - 0.55f * t0), rB = r0 * (1.0f - 0.55f * t1);
        const glm::vec3 col = band ? dark * 1.4f : white, mid = (at(t0) + at(t1)) * 0.5f;
        for (int k = 0; k < sides; ++k)
            addQuad(m, ring(t0, rA, k), ring(t0, rA, k + 1), ring(t1, rB, k + 1), ring(t1, rB, k), mid, col, kPlainMaterial);
    }
    // Две-три тонкие ветки вверх и в стороны.
    for (int k = 0; k < 3; ++k) {
        const float t = 0.5f + 0.12f * static_cast<float>(k), a = rrange(1, v, 70 + k, 0.0f, 6.28f);
        const glm::vec3 from = at(t), to = from + glm::vec3(std::cos(a) * 0.9f, 0.9f, std::sin(a) * 0.9f);
        addTaperedPrism(m, from, to, r0 * 0.3f, r0 * 0.14f, 4, white * 0.92f);
    }
    // Узкая светлая крона: три вытянутых комка друг над другом, смещены по лёгкому изгибу.
    const glm::vec3 leaf{0.46f, 0.60f, 0.18f};
    for (int k = 0; k < 3; ++k) {
        const float t = 0.62f + 0.19f * static_cast<float>(k);
        const glm::vec3 c = at(t) + glm::vec3(rrange(1, v, 80 + k, -0.35f, 0.35f), 0.35f, rrange(1, v, 83 + k, -0.35f, 0.35f));
        const float r = (k == 1 ? 1.05f : (k == 0 ? 0.95f : 0.7f)) * rrange(1, v, 86 + k, 0.9f, 1.1f);
        blobT(m, solids, c, {r, r * 1.35f, r}, leaf * (0.92f + 0.18f * rnd(1, v, 89 + k)), 0.5f, 100 + 7 * k + v, k == 1 ? 1 : 0, kMottled);
    }
    removeBuriedTriangles(m, solids, 1.0f);
    return m;
}

}  // namespace

MeshData buildTree(TreeSpecies species, int variant) {
    variant = std::clamp(variant, 0, kTreeVariants - 1);
    return species == TreeSpecies::Birch ? birch(variant) : oak(variant);
}

}  // namespace wsim::procgen

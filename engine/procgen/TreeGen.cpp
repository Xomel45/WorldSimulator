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
    blobT(m, solids, {top.x, cy + 0.3f, top.z}, {2.2f, 1.7f, 2.2f}, leaf, 0.55f, 1 + v, 1, kLeaf);
    for (int k = 0; k < 3; ++k) {
        const float a = rrange(0, v, 20 + k, 0.0f, 6.28f), d = rrange(0, v, 23 + k, 1.2f, 1.7f);
        blobT(m, solids, {top.x + std::cos(a) * d, cy - 0.15f + 0.3f * rnd(0, v, 26 + k), top.z + std::sin(a) * d},
                {1.45f, 1.15f, 1.45f}, leaf * (0.9f + 0.2f * rnd(0, v, 30 + k)), 0.5f, 10 + 3 * k + v, 1, kLeaf);
    }
    blobT(m, solids, {top.x + rrange(0, v, 40, -0.5f, 0.5f), cy + 1.3f, top.z + rrange(0, v, 41, -0.5f, 0.5f)}, {1.2f, 0.85f, 1.2f},
            leaf * 1.12f, 0.5f, 50 + v, 0, kLeaf);
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
        blobT(m, solids, c, {r, r * 1.35f, r}, leaf * (0.92f + 0.18f * rnd(1, v, 89 + k)), 0.5f, 100 + 7 * k + v, k == 1 ? 1 : 0, kLeaf);
    }
    removeBuriedTriangles(m, solids, 1.0f);
    return m;
}

// Ель: прямой тёмный ствол и пять ярусов хвои-конусов, нижние шире; ярусы чуть сдвинуты и повёрнуты.
MeshData spruce(int v) {
    MeshData m;
    const glm::vec3 bark{0.26f, 0.18f, 0.12f};
    const float height = rrange(2, v, 1, 4.6f, 5.8f), r0 = rrange(2, v, 2, 0.17f, 0.23f);
    const float leanX = rrange(2, v, 3, -0.12f, 0.12f), leanZ = rrange(2, v, 4, -0.12f, 0.12f);
    addTube(m, {{0, -0.5f, 0}, {leanX * 0.5f, height * 0.5f, leanZ * 0.5f}, {leanX, height, leanZ}}, {r0 * 1.2f, r0 * 0.9f, r0 * 0.4f}, {bark * 0.85f, bark}, 6);
    const int   tiers = 5;
    const float base0 = 0.9f, step = (height - 1.1f) / static_cast<float>(tiers);
    for (int k = 0; k < tiers; ++k) {
        const float t = static_cast<float>(k) / static_cast<float>(tiers - 1);
        const float rBase = (1.75f - 1.15f * t) * rrange(2, v, 10 + k, 0.92f, 1.08f);
        const float y0 = base0 + step * static_cast<float>(k);
        const glm::vec3 axis{leanX * (y0 / height), y0, leanZ * (y0 / height)};
        const glm::vec3 col = glm::vec3(0.06f, 0.21f, 0.12f) * (0.9f + 0.25f * rnd(2, v, 20 + k)) + glm::vec3(0.02f, 0.03f, 0.0f) * t;
        addTaperedPrism(m, axis, axis + glm::vec3(0, step * 1.9f + 0.4f, 0), rBase, 0.03f, 7, col, kMottled, rrange(2, v, 30 + k, 0.0f, 6.28f));
    }
    return m;
}

// Акация: тонкий изогнутый ствол с развилкой и плоская широкая "зонтичная" крона из двух приплюснутых комков.
MeshData acacia(int v) {
    MeshData m;
    std::vector<SolidEllipsoid> solids;
    const glm::vec3 bark{0.36f, 0.27f, 0.17f};
    const float height = rrange(3, v, 1, 3.0f, 3.8f), r0 = rrange(3, v, 2, 0.17f, 0.22f);
    const float lean = rrange(3, v, 3, -0.5f, 0.5f);
    const glm::vec3 fork{lean, height, rrange(3, v, 4, -0.3f, 0.3f)};
    addTube(m, {{0, -0.5f, 0}, {lean * 0.3f, height * 0.5f, 0}, fork}, {r0 * 1.2f, r0 * 0.9f, r0 * 0.7f}, {bark * 0.85f, bark}, 6);
    for (int k = 0; k < 2; ++k) {
        const float a = rrange(3, v, 10 + k, 0.0f, 6.28f) + 3.14f * static_cast<float>(k);
        addTaperedPrism(m, fork - glm::vec3(0, 0.2f, 0), fork + glm::vec3(std::cos(a) * 1.0f, 0.75f, std::sin(a) * 1.0f), r0 * 0.5f, r0 * 0.28f, 5, bark);
    }
    const glm::vec3 leaf{0.40f, 0.48f, 0.15f};
    const float cy = height + 0.7f;
    blobT(m, solids, {fork.x, cy, fork.z}, {2.9f, 0.55f, 2.7f}, leaf, 0.35f, 200 + v, 1, kLeaf);
    blobT(m, solids, {fork.x + rrange(3, v, 20, -0.9f, 0.9f), cy + 0.35f, fork.z + rrange(3, v, 21, -0.9f, 0.9f)}, {1.8f, 0.4f, 1.7f}, leaf * 1.1f, 0.35f, 210 + v, 0, kLeaf);
    removeBuriedTriangles(m, solids, 1.0f);
    return m;
}

// Кактус (сагуаро): ребристая колонна с округлой макушкой и две руки -- горизонтальный отрог и вверх.
MeshData cactus(int v) {
    MeshData m;
    const glm::vec3 green{0.20f, 0.40f, 0.20f};
    const float height = rrange(4, v, 1, 2.2f, 3.0f), r = rrange(4, v, 2, 0.24f, 0.30f);
    addTube(m, {{0, -0.4f, 0}, {0, height * 0.5f, 0}, {0, height, 0}}, {r * 1.1f, r, r * 0.85f}, {green * 0.9f, green}, 8);
    addBlob(m, {0, height + 0.02f, 0}, {r * 0.85f, r * 0.85f, r * 0.85f}, green * 1.05f, 0.1f, 300 + v, 0, kPlainMaterial);
    for (int k = 0; k < 2; ++k) {
        const float a  = rrange(4, v, 10 + k, 0.0f, 6.28f) + 3.14f * static_cast<float>(k);
        const float y0 = height * rrange(4, v, 14 + k, 0.35f, 0.55f), reach = rrange(4, v, 16 + k, 0.55f, 0.8f), up = rrange(4, v, 18 + k, 0.7f, 1.1f);
        const glm::vec3 d(std::cos(a), 0, std::sin(a));
        const glm::vec3 p0{0, y0, 0}, p1 = d * reach + glm::vec3(0, y0, 0), p2 = p1 + glm::vec3(0, up, 0);
        // Две прямые призмы и шар в колене: у трубы через изгиб в 90 градусов грани перекручиваются.
        addTaperedPrism(m, p0, p1, r * 0.6f, r * 0.58f, 6, green);
        addTaperedPrism(m, p1, p2, r * 0.58f, r * 0.5f, 6, green * 1.03f);
        addBlob(m, p1, {r * 0.62f, r * 0.62f, r * 0.62f}, green, 0.08f, 340 + 3 * k + v, 0, kPlainMaterial);
        addBlob(m, p2 + glm::vec3(0, 0.02f, 0), {r * 0.5f, r * 0.5f, r * 0.5f}, green * 1.05f, 0.1f, 320 + 3 * k + v, 0, kPlainMaterial);
    }
    return m;
}

// Пальма: изогнутый светлый ствол с кольцами и венок из восьми дугообразных листьев.
MeshData palm(int v) {
    MeshData m;
    const glm::vec3 trunk{0.52f, 0.41f, 0.27f};
    const float height = rrange(5, v, 1, 4.4f, 5.4f), r0 = rrange(5, v, 2, 0.17f, 0.22f);
    const float lx = rrange(5, v, 3, -0.9f, 0.9f), lz = rrange(5, v, 4, -0.9f, 0.9f);
    auto at = [&](float t) { return glm::vec3(lx * t * t, -0.5f + (height + 0.5f) * t, lz * t * t); };
    std::vector<glm::vec3> pts;
    std::vector<float>     radii;
    std::vector<glm::vec3> cols;
    for (int s = 0; s <= 6; ++s) {
        const float t = static_cast<float>(s) / 6.0f;
        pts.push_back(at(t));
        radii.push_back(r0 * (1.15f - 0.45f * t));
        if (s < 6) cols.push_back(trunk * (s % 2 == 0 ? 0.88f : 1.08f));  // кольца-"рубцы"
    }
    addTube(m, pts, radii, cols, 6);
    const glm::vec3 top = at(1.0f);
    const glm::vec3 leaf{0.14f, 0.40f, 0.12f};
    const int fronds = 8;
    for (int k = 0; k < fronds; ++k) {
        const float a = 6.2831853f * static_cast<float>(k) / static_cast<float>(fronds) + rrange(5, v, 10, 0.0f, 0.5f);
        const glm::vec3 d(std::cos(a), 0, std::sin(a)), side = glm::vec3(-d.z, 0, d.x);
        const float len = rrange(5, v, 20 + k, 1.9f, 2.5f), w = 0.38f;
        const glm::vec3 mid = top + d * (len * 0.5f) + glm::vec3(0, 0.55f, 0);
        const glm::vec3 tip = top + d * len + glm::vec3(0, -0.45f, 0);
        const glm::vec3 inside = top - glm::vec3(0, 1.0f, 0);
        const glm::vec3 col = leaf * (0.92f + 0.2f * rnd(5, v, 40 + k));
        addTri(m, top, mid + side * w, mid - side * w, inside, col, kMottled);   // два плоских треугольника: лист -- ромб
        addTri(m, mid + side * w, tip, mid - side * w, inside, col, kMottled);
    }
    return m;
}

// Сосна: высокий стройный ствол, внизу тёмный, а выше рыжий; голый до самой кроны. Крона -- неровная "шапка" из
// нескольких плоских комков сине-зелёной хвои, чуть набок; пара коротких веток под ней.
MeshData pine(int v) {
    MeshData m;
    std::vector<SolidEllipsoid> solids;
    const glm::vec3 low{0.34f, 0.22f, 0.15f}, high{0.64f, 0.38f, 0.21f};
    const float height = rrange(6, v, 1, 5.8f, 7.0f), r0 = rrange(6, v, 2, 0.17f, 0.22f);
    const float leanX = rrange(6, v, 3, -0.45f, 0.45f), leanZ = rrange(6, v, 4, -0.45f, 0.45f);
    auto at = [&](float t) { return glm::vec3(leanX * t * t, -0.5f + (height + 0.5f) * t, leanZ * t * t); };
    addTube(m, {at(0.0f), at(0.35f), at(0.7f), at(1.0f)}, {r0 * 1.2f, r0 * 0.95f, r0 * 0.7f, r0 * 0.45f}, {low, glm::mix(low, high, 0.6f), high}, 7);
    // Пара коротких сучьев под кроной.
    for (int k = 0; k < 2; ++k) {
        const float a = rrange(6, v, 10 + k, 0.0f, 6.28f), t = 0.68f + 0.1f * static_cast<float>(k);
        const glm::vec3 from = at(t), to = from + glm::vec3(std::cos(a) * 0.9f, 0.5f, std::sin(a) * 0.9f);
        addTaperedPrism(m, from, to, r0 * 0.4f, r0 * 0.16f, 4, high * 0.9f);
    }
    // Крона: низкий широкий комок, над ним два поменьше и один боковой -- "шапка" с неровным краем.
    const glm::vec3 needles{0.11f, 0.30f, 0.17f};
    const glm::vec3 top = at(1.0f);
    blobT(m, solids, top + glm::vec3(0.0f, 0.55f, 0.0f), {1.9f, 0.8f, 1.9f}, needles, 0.5f, 400 + v, 1, kMottled);
    blobT(m, solids, top + glm::vec3(rrange(6, v, 20, -0.5f, 0.5f), 1.25f, rrange(6, v, 21, -0.5f, 0.5f)), {1.3f, 0.65f, 1.3f},
            needles * 1.12f, 0.5f, 410 + v, 0, kMottled);
    const float a = rrange(6, v, 22, 0.0f, 6.28f);
    blobT(m, solids, top + glm::vec3(std::cos(a) * 1.3f, 0.2f, std::sin(a) * 1.3f), {1.1f, 0.55f, 1.1f}, needles * 0.92f, 0.5f, 420 + v, 0, kMottled);
    blobT(m, solids, top + glm::vec3(-std::cos(a) * 1.0f, 0.35f, -std::sin(a) * 1.0f), {0.95f, 0.5f, 0.95f}, needles * 1.05f, 0.5f, 430 + v, 0, kMottled);
    removeBuriedTriangles(m, solids, 1.0f);
    return m;
}

}  // namespace

MeshData buildTree(TreeSpecies species, int variant) {
    variant = std::clamp(variant, 0, kTreeVariants - 1);
    switch (species) {
        case TreeSpecies::Birch:  return birch(variant);
        case TreeSpecies::Spruce: return spruce(variant);
        case TreeSpecies::Pine:   return pine(variant);
        case TreeSpecies::Acacia: return acacia(variant);
        case TreeSpecies::Cactus: return cactus(variant);
        case TreeSpecies::Palm:   return palm(variant);
        case TreeSpecies::Oak:    break;
    }
    return oak(variant);
}

}  // namespace wsim::procgen

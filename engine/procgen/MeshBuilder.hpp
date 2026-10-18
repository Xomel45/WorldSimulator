#pragma once

#include "renderer/Vertex.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wsim::procgen {

// Простые строительные блоки процедурных мешей: замкнутая геометрия с внешними
// нормалями (CCW снаружи, как требует cullMode=BACK сцены). Приёмы взяты из
// PodjezdGen (addBox/addBeam), но цвет теперь несёт код материала в альфе
// (см. assets/shaders/include/materials.glsl): 1.0 -- цвет вершины как есть,
// 0.75 -- пятнистая "шкура/штукатурка".
struct MeshData {
    std::vector<Vertex>        vertices;
    std::vector<std::uint32_t> indices;
};

inline constexpr float kPlainMaterial = 1.0f;
inline constexpr float kMottled       = 0.75f;

// Грани бокса, которые можно не строить (маска `skip` у addBox): те, что прижаты к другой геометрии или
// ушли под землю и снаружи не видны.
enum BoxFace : unsigned { kFacePosY = 1, kFaceNegY = 2, kFacePosZ = 4, kFaceNegZ = 8, kFacePosX = 16, kFaceNegX = 32 };

// Осепараллельный бокс [mn, mx]. skip -- маска BoxFace граней, которых не будет (скрыты внутри/под другим объектом).
inline void addBox(MeshData& m, glm::vec3 mn, glm::vec3 mx, glm::vec3 color, float material = kPlainMaterial, unsigned skip = 0) {
    const glm::vec4 c(color, material);
    auto face = [&](glm::vec3 o, glm::vec3 e1, glm::vec3 e2) {
        const glm::vec4 n4(glm::normalize(glm::cross(e1, e2)), 0.0f);
        const auto      base = static_cast<std::uint32_t>(m.vertices.size());
        m.vertices.push_back({glm::vec4(o, 0.0f), n4, c});
        m.vertices.push_back({glm::vec4(o + e1, 0.0f), n4, c});
        m.vertices.push_back({glm::vec4(o + e1 + e2, 0.0f), n4, c});
        m.vertices.push_back({glm::vec4(o + e2, 0.0f), n4, c});
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    const glm::vec3 d = mx - mn;
    const glm::vec3 ex(d.x, 0, 0), ey(0, d.y, 0), ez(0, 0, d.z);
    if (!(skip & kFacePosY)) face({mn.x, mx.y, mn.z}, ez, ex);
    if (!(skip & kFaceNegY)) face({mn.x, mn.y, mn.z}, ex, ez);
    if (!(skip & kFacePosZ)) face({mn.x, mn.y, mx.z}, ex, ey);
    if (!(skip & kFaceNegZ)) face({mx.x, mn.y, mn.z}, -ex, ey);
    if (!(skip & kFacePosX)) face({mx.x, mn.y, mx.z}, -ez, ey);
    if (!(skip & kFaceNegX)) face({mn.x, mn.y, mn.z}, ez, ey);
}

// Эллипсоид "заведомо сплошного" тела (внутренность кроны и т.п.) и диапазон его собственных треугольников
// [firstTri, lastTri) -- они не проверяются на скрытость внутри него самого.
struct SolidEllipsoid {
    glm::vec3   center, radii;
    std::size_t firstTri = 0, lastTri = 0;
};

// Убирает треугольники, все три вершины которых лежат глубоко внутри чужого эллипсоида (нормированный радиус
// < `depth`): снаружи они не видны. depth -- с запасом на шероховатость поверхности. Возвращает число убранных.
inline std::size_t removeBuriedTriangles(MeshData& m, const std::vector<SolidEllipsoid>& solids, float depth = 0.68f) {
    const std::size_t triCount = m.indices.size() / 3;
    std::vector<std::uint32_t> keptIdx;
    std::vector<Vertex>        keptVerts;
    keptIdx.reserve(m.indices.size());
    keptVerts.reserve(m.vertices.size());
    std::size_t removed = 0;
    for (std::size_t t = 0; t < triCount; ++t) {
        bool buried = false;
        for (const SolidEllipsoid& e : solids) {
            if (t >= e.firstTri && t < e.lastTri) continue;
            bool all = true;
            for (int k = 0; k < 3 && all; ++k) {
                const glm::vec3 p = glm::vec3(m.vertices[m.indices[t * 3 + static_cast<std::size_t>(k)]].position);
                all = glm::length((p - e.center) / e.radii) < depth;
            }
            if (all) { buried = true; break; }
        }
        if (buried) { ++removed; continue; }
        for (int k = 0; k < 3; ++k) {
            keptIdx.push_back(static_cast<std::uint32_t>(keptVerts.size()));
            keptVerts.push_back(m.vertices[m.indices[t * 3 + static_cast<std::size_t>(k)]]);
        }
    }
    m.vertices = std::move(keptVerts);
    m.indices  = std::move(keptIdx);
    return removed;
}

// Произвольный выпуклый многоугольник-четырёхугольник; нормаль и порядок вершин
// ориентируются наружу робастно -- от центра тела `inside` (точка внутри тела).
inline void addQuad(MeshData& m, glm::vec3 p0, glm::vec3 p1, glm::vec3 p2, glm::vec3 p3, const glm::vec3& inside,
                    glm::vec3 color, float material = kPlainMaterial) {
    glm::vec3 n = glm::cross(p1 - p0, p3 - p0);
    if (glm::length(n) < 1.0e-8f) return;
    n = glm::normalize(n);
    const glm::vec3 center = (p0 + p1 + p2 + p3) * 0.25f;
    if (glm::dot(n, center - inside) < 0.0f) { std::swap(p1, p3); n = -n; }  // развернуть наружу
    const glm::vec4 c(color, material), n4(n, 0.0f);
    const auto      base = static_cast<std::uint32_t>(m.vertices.size());
    for (const glm::vec3& p : {p0, p1, p2, p3}) m.vertices.push_back({glm::vec4(p, 0.0f), n4, c});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

inline void addTri(MeshData& m, glm::vec3 a, glm::vec3 b, glm::vec3 c3, const glm::vec3& inside, glm::vec3 color,
                   float material = kPlainMaterial) {
    glm::vec3 n = glm::cross(b - a, c3 - a);
    if (glm::length(n) < 1.0e-8f) return;
    n = glm::normalize(n);
    if (glm::dot(n, (a + b + c3) / 3.0f - inside) < 0.0f) { std::swap(b, c3); n = -n; }
    const glm::vec4 c(color, material), n4(n, 0.0f);
    const auto      base = static_cast<std::uint32_t>(m.vertices.size());
    for (const glm::vec3& p : {a, b, c3}) m.vertices.push_back({glm::vec4(p, 0.0f), n4, c});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
}

// Двускатная крыша: призма над прямоугольником [mn.xz, mx.xz] от высоты mn.y до гребня mx.y,
// гребень вдоль оси Z (ridgeAlongZ) или X. overhang -- свес карниза.
inline void addGabledRoof(MeshData& m, glm::vec3 mn, glm::vec3 mx, float overhang, bool ridgeAlongZ, glm::vec3 color,
                          glm::vec3 gableColor, float material = kPlainMaterial) {
    const float x0 = mn.x - overhang, x1 = mx.x + overhang, z0 = mn.z - overhang, z1 = mx.z + overhang;
    const float yb = mn.y, yt = mx.y;
    const glm::vec3 inside((x0 + x1) * 0.5f, (yb + yt) * 0.5f, (z0 + z1) * 0.5f);
    if (ridgeAlongZ) {
        const float xm = (x0 + x1) * 0.5f;
        addQuad(m, {x0, yb, z0}, {xm, yt, z0}, {xm, yt, z1}, {x0, yb, z1}, inside, color, material);  // левый скат
        addQuad(m, {x1, yb, z0}, {xm, yt, z0}, {xm, yt, z1}, {x1, yb, z1}, inside, color, material);  // правый скат
        addTri(m, {x0, yb, z0}, {x1, yb, z0}, {xm, yt, z0}, inside, gableColor, material);            // фронтоны
        addTri(m, {x0, yb, z1}, {x1, yb, z1}, {xm, yt, z1}, inside, gableColor, material);
        addQuad(m, {x0, yb, z0}, {x1, yb, z0}, {x1, yb, z1}, {x0, yb, z1}, inside, gableColor, material);  // низ (чтобы призма была замкнута)
    } else {
        const float zm = (z0 + z1) * 0.5f;
        addQuad(m, {x0, yb, z0}, {x0, yt, zm}, {x1, yt, zm}, {x1, yb, z0}, inside, color, material);
        addQuad(m, {x0, yb, z1}, {x0, yt, zm}, {x1, yt, zm}, {x1, yb, z1}, inside, color, material);
        addTri(m, {x0, yb, z0}, {x0, yb, z1}, {x0, yt, zm}, inside, gableColor, material);
        addTri(m, {x1, yb, z0}, {x1, yb, z1}, {x1, yt, zm}, inside, gableColor, material);
        addQuad(m, {x0, yb, z0}, {x1, yb, z0}, {x1, yb, z1}, {x0, yb, z1}, inside, gableColor, material);
    }
}

// Усечённая призма-"ствол" между точками base и top с радиусами rBase/rTop и `sides` гранями (боковые
// грани; торцы не нужны -- низ под землёй, верх в кроне). Стороны ориентируются наружу от оси.
inline void addTaperedPrism(MeshData& m, glm::vec3 base, glm::vec3 top, float rBase, float rTop, int sides, glm::vec3 color,
                            float material = kPlainMaterial, float phase = 0.0f) {
    const glm::vec3 axis = glm::normalize(top - base);
    const glm::vec3 ref  = std::abs(axis.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    const glm::vec3 u    = glm::normalize(glm::cross(axis, ref)), w = glm::cross(axis, u);
    const glm::vec3 mid  = (base + top) * 0.5f;
    for (int k = 0; k < sides; ++k) {
        const float a0 = phase + 6.2831853f * static_cast<float>(k) / static_cast<float>(sides);
        const float a1 = phase + 6.2831853f * static_cast<float>(k + 1) / static_cast<float>(sides);
        const glm::vec3 d0 = u * std::cos(a0) + w * std::sin(a0), d1 = u * std::cos(a1) + w * std::sin(a1);
        addQuad(m, base + d0 * rBase, base + d1 * rBase, top + d1 * rTop, top + d0 * rTop, mid, color, material);
    }
}

// Неровный "комок" листвы: икосфера (levels 0 -- 20 граней, 1 -- 80), растянутая по radii и с радиальным шумом,
// плоская закраска гранями. Детерминирован по seed.
inline void addBlob(MeshData& m, glm::vec3 center, glm::vec3 radii, glm::vec3 color, float roughness, std::uint32_t seed,
                    int levels = 1, float material = kPlainMaterial, float toneVar = 0.35f) {
    std::vector<glm::vec3> pts;
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    for (glm::vec3 v : {glm::vec3{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
                        {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}})
        pts.push_back(glm::normalize(v));
    std::vector<std::array<int, 3>> faces = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2},
                                             {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5},
                                             {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (int level = 0; level < levels; ++level) {
        std::vector<std::pair<std::pair<int, int>, int>> mid;
        auto midpoint = [&](int a, int b) {
            const auto key = std::minmax(a, b);
            for (const auto& e : mid) if (e.first == key) return e.second;
            pts.push_back(glm::normalize(pts[static_cast<std::size_t>(a)] + pts[static_cast<std::size_t>(b)]));
            mid.push_back({key, static_cast<int>(pts.size()) - 1});
            return static_cast<int>(pts.size()) - 1;
        };
        std::vector<std::array<int, 3>> next;
        for (const auto& f : faces) {
            const int ab = midpoint(f[0], f[1]), bc = midpoint(f[1], f[2]), ca = midpoint(f[2], f[0]);
            next.push_back({f[0], ab, ca}); next.push_back({f[1], bc, ab}); next.push_back({f[2], ca, bc}); next.push_back({ab, bc, ca});
        }
        faces = std::move(next);
    }
    auto hash = [seed](const glm::vec3& p) {
        const float h = std::sin(p.x * 127.1f + p.y * 311.7f + p.z * 74.7f + static_cast<float>(seed) * 19.19f) * 43758.5453f;
        return h - std::floor(h);
    };
    for (auto& p : pts) p *= (1.0f - 0.5f * roughness) + roughness * hash(p);
    for (const auto& f : faces) {
        const glm::vec3 a = center + pts[static_cast<std::size_t>(f[0])] * radii, b = center + pts[static_cast<std::size_t>(f[1])] * radii,
                        c = center + pts[static_cast<std::size_t>(f[2])] * radii;
        const float tone = 1.0f - toneVar * 0.5f + toneVar * hash(pts[static_cast<std::size_t>(f[0])] + pts[static_cast<std::size_t>(f[1])]);
        glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
        glm::vec3 b2 = b, c2 = c;
        if (glm::dot(n, (a + b + c) / 3.0f - center) < 0.0f) { std::swap(b2, c2); n = -n; }  // CCW снаружи
        const glm::vec4 col(color * tone, material), n4(n, 0.0f);
        const auto base = static_cast<std::uint32_t>(m.vertices.size());
        for (const glm::vec3& v : {a, b2, c2}) m.vertices.push_back({glm::vec4(v, 0.0f), n4, col});
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
    }
}

}  // namespace wsim::procgen

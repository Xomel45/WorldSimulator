#include "procgen/HouseGen.hpp"

#include <algorithm>

namespace wsim::procgen {

namespace {
// Маленький детерминированный генератор по зерну: без <random>, результат одинаков на всех платформах.
struct Lcg {
    std::uint32_t s;
    float next() {  // 0..1
        s = s * 1664525u + 1013904223u;
        return static_cast<float>((s >> 8) & 0xFFFFFF) / static_cast<float>(0x1000000);
    }
    float range(float a, float b) { return a + (b - a) * next(); }
};
}  // namespace

HouseParams houseVariant(std::uint32_t seed) {
    Lcg r{seed * 2654435761u + 12345u};
    HouseParams p;
    p.width       = r.range(3.4f, 5.2f);
    p.depth       = r.range(4.2f, 6.4f);
    p.wallHeight  = r.range(2.3f, 3.0f);
    p.roofHeight  = r.range(1.3f, 2.4f);
    p.ridgeAlongZ = r.next() > 0.35f;
    p.chimney     = r.next() > 0.3f;
    p.windows     = 1 + static_cast<int>(r.next() * 2.99f);
    const glm::vec3 walls[] = {{0.80f, 0.72f, 0.58f}, {0.72f, 0.74f, 0.70f}, {0.84f, 0.78f, 0.64f}, {0.62f, 0.52f, 0.42f}, {0.78f, 0.62f, 0.50f}};
    const glm::vec3 roofs[] = {{0.52f, 0.22f, 0.16f}, {0.30f, 0.30f, 0.34f}, {0.40f, 0.28f, 0.18f}, {0.20f, 0.34f, 0.28f}};
    p.wall = walls[static_cast<std::size_t>(r.next() * 4.99f)];
    p.roof = roofs[static_cast<std::size_t>(r.next() * 3.99f)];
    return p;
}

MeshData buildHouse(const HouseParams& p) {
    MeshData m;
    const float hw = p.width * 0.5f, hd = p.depth * 0.5f, H = p.wallHeight;
    const float plinth = 0.25f;  // цоколь выступает на 5 см и уходит под землю: дом стоит на неровном месте без щелей

    // Цоколь заглублён под землю, чтобы на склоне не было просвета.
    // Грани, скрытые под землёй/внутри соседних частей, не строятся (kFace* -- см. MeshBuilder.hpp).
    addBox(m, {-hw - 0.05f, -1.2f, -hd - 0.05f}, {hw + 0.05f, plinth, hd + 0.05f}, p.trim, kPlainMaterial, kFaceNegY);
    addBox(m, {-hw, plinth, -hd}, {hw, H, hd}, p.wall, kMottled, kFaceNegY | kFacePosY);  // низ на цоколе, верх под крышей

    // Крыша.
    addGabledRoof(m, {-hw, H, -hd}, {hw, H + p.roofHeight, hd}, p.overhang, p.ridgeAlongZ, p.roof, p.wall, kMottled);

    // Дверь на фасаде (+Z): косяк и тёмное полотно чуть выступают.
    const float dw = 0.9f, dh = 1.9f, dz = hd;
    addBox(m, {-dw * 0.5f - 0.1f, plinth, dz}, {dw * 0.5f + 0.1f, dh + 0.1f, dz + 0.06f}, p.trim, kPlainMaterial,
           kFaceNegY | kFaceNegZ);  // низ на цоколе, тыл у стены
    addBox(m, {-dw * 0.5f, plinth, dz + 0.06f}, {dw * 0.5f, dh, dz + 0.1f}, p.trim * 0.6f, kPlainMaterial,
           kFaceNegY | kFaceNegZ);  // тыл -- в косяке

    // Окна на боковых стенах (±X): рама и стекло.
    for (int side : {-1, 1}) {
        for (int i = 0; i < p.windows; ++i) {
            const float zc = (static_cast<float>(i) + 0.5f) / static_cast<float>(p.windows) * p.depth - hd;
            const float x0 = side > 0 ? hw : -hw - 0.06f, x1 = side > 0 ? hw + 0.06f : -hw;
            const unsigned toWall = side > 0 ? kFaceNegX : kFacePosX;  // сторона рамы, прижатая к стене
            addBox(m, {x0, 1.15f, zc - 0.52f}, {x1, 2.05f, zc + 0.52f}, p.trim, kPlainMaterial, toWall);
            const float gx0 = side > 0 ? hw + 0.02f : -hw - 0.08f, gx1 = side > 0 ? hw + 0.08f : -hw - 0.02f;
            addBox(m, {gx0, 1.25f, zc - 0.42f}, {gx1, 1.95f, zc + 0.42f}, p.glass, kPlainMaterial, toWall);
        }
    }

    // Труба на скате.
    if (p.chimney) {
        const float cx = p.ridgeAlongZ ? hw * 0.45f : 0.0f, cz = p.ridgeAlongZ ? -hd * 0.3f : -hd * 0.45f;
        addBox(m, {cx - 0.3f, H + 0.3f, cz - 0.3f}, {cx + 0.3f, H + p.roofHeight + 0.6f, cz + 0.3f}, p.trim, kPlainMaterial, kFaceNegY);
    }
    return m;
}

}  // namespace wsim::procgen

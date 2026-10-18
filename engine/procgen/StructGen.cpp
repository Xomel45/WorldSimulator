#include "procgen/StructGen.hpp"

#include <algorithm>

namespace wsim::procgen {

namespace {
constexpr glm::vec3 kWood{0.52f, 0.34f, 0.18f}, kDarkWood{0.34f, 0.21f, 0.11f}, kLightWood{0.64f, 0.46f, 0.26f}, kIron{0.66f, 0.69f, 0.73f};
glm::vec3 tint(glm::vec3 c, float k) { return glm::clamp(c * k, 0.0f, 1.0f); }
}  // namespace

MeshData buildPlank() {
    MeshData m;
    // Четыре доски вдоль Z с зазорами и две балки поперёк снизу.
    for (int i = 0; i < 4; ++i) {
        const float x0 = -1.0f + 0.5f * static_cast<float>(i) + 0.03f;
        addBox(m, {x0, -0.16f, -1.0f}, {x0 + 0.44f, 0.0f, 1.0f}, tint(kWood, 0.92f + 0.05f * static_cast<float>(i % 2)));
    }
    addBox(m, {-1.0f, -0.32f, -0.7f}, {1.0f, -0.16f, -0.5f}, kDarkWood);
    addBox(m, {-1.0f, -0.32f, 0.5f}, {1.0f, -0.16f, 0.7f}, kDarkWood);
    return m;
}

MeshData buildBarricade() {
    MeshData m;
    constexpr int kStakes = 7;
    for (int i = 0; i < kStakes; ++i) {
        const float x = -1.65f + 3.3f * static_cast<float>(i) / (kStakes - 1);
        const float h = 1.0f + 0.22f * static_cast<float>((i * 5) % 3);
        const glm::vec3 c = tint(kWood, 0.9f + 0.06f * static_cast<float>(i % 3));
        addBox(m, {x - 0.13f, -0.2f, -0.13f}, {x + 0.13f, h, 0.13f}, c);
        addBox(m, {x - 0.07f, h, -0.07f}, {x + 0.07f, h + 0.22f, 0.07f}, tint(kLightWood, 1.0f));   // заострённый кончик
    }
    addBox(m, {-1.8f, 0.30f, 0.10f}, {1.8f, 0.44f, 0.30f}, kDarkWood);
    addBox(m, {-1.8f, 0.68f, -0.30f}, {1.8f, 0.82f, -0.10f}, kDarkWood);
    return m;
}

MeshData buildTrap() {
    MeshData m;
    addBox(m, {-0.7f, 0.0f, -0.7f}, {0.7f, 0.1f, 0.7f}, kDarkWood);
    for (int i = 0; i < 9; ++i) {
        const float x = -0.4f + 0.4f * static_cast<float>(i % 3), z = -0.4f + 0.4f * static_cast<float>(i / 3);
        addBox(m, {x - 0.05f, 0.1f, z - 0.05f}, {x + 0.05f, 0.5f, z + 0.05f}, kWood);
        addBox(m, {x - 0.025f, 0.5f, z - 0.025f}, {x + 0.025f, 0.62f, z + 0.025f}, kIron);
    }
    return m;
}

MeshData buildSpear() {
    MeshData m;
    addBox(m, {-0.03f, -0.03f, -0.5f}, {0.03f, 0.03f, 1.25f}, kLightWood);
    addBox(m, {-0.05f, -0.05f, 1.25f}, {0.05f, 0.05f, 1.5f}, kIron);
    addBox(m, {-0.025f, -0.025f, 1.5f}, {0.025f, 0.025f, 1.65f}, kIron);
    return m;
}

MeshData buildShield() {
    MeshData m;
    addBox(m, {-0.04f, 0.0f, -0.3f}, {0.04f, 0.65f, 0.3f}, tint(kWood, 1.05f));
    addBox(m, {-0.07f, 0.28f, -0.08f}, {0.07f, 0.37f, 0.08f}, kIron);       // умбон
    addBox(m, {-0.055f, 0.0f, -0.04f}, {0.055f, 0.65f, 0.04f}, kDarkWood);  // средняя планка
    return m;
}

MeshData buildLogs(int count) {
    MeshData m;
    count = std::clamp(count, 1, 4);
    // Брёвна лежат вдоль X, друг на друге по два в ряд.
    for (int i = 0; i < count; ++i) {
        const float z = (i % 2 == 0) ? -0.1f : 0.1f, y = 0.0f + 0.17f * static_cast<float>(i / 2) + (i % 2 ? 0.0f : 0.0f);
        addBox(m, {-0.45f, y - 0.07f, z - 0.07f}, {0.45f, y + 0.07f, z + 0.07f}, tint(kWood, 0.9f + 0.05f * static_cast<float>(i)));
        addBox(m, {0.45f, y - 0.05f, z - 0.05f}, {0.48f, y + 0.05f, z + 0.05f}, kLightWood);   // торец
    }
    return m;
}

}  // namespace wsim::procgen

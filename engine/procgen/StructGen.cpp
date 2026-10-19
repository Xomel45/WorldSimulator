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

MeshData buildStockpile() {
    MeshData m;
    addBox(m, {-1.4f, 0.0f, -1.0f}, {1.4f, 0.14f, 1.0f}, kDarkWood);   // помост
    for (int row = 0; row < 3; ++row)                                   // штабель: три яруса брёвен вдоль X
        for (int i = 0; i < 4 - row; ++i) {
            const float z = -0.62f + 0.31f * static_cast<float>(i) + 0.155f * static_cast<float>(row), y = 0.14f + 0.28f * static_cast<float>(row);
            addBox(m, {-1.1f, y, z - 0.14f}, {1.1f, y + 0.28f, z + 0.14f}, tint(kWood, 0.88f + 0.06f * static_cast<float>((i + row) % 3)));
            addBox(m, {1.1f, y + 0.05f, z - 0.1f}, {1.14f, y + 0.23f, z + 0.1f}, kLightWood);
        }
    for (float x : {-1.3f, 1.3f}) addBox(m, {x - 0.08f, 0.14f, 0.78f}, {x + 0.08f, 1.6f, 0.94f}, kWood);   // столбы навеса
    addBox(m, {-1.45f, 1.6f, 0.5f}, {1.45f, 1.72f, 1.2f}, tint(kLightWood, 0.95f));                      // кровля
    return m;
}

MeshData buildHut() {
    MeshData m;
    constexpr float kW = 1.1f, kD = 1.1f, kH = 1.15f;
    // Стены: ярусы брёвен (по 4 слоя), у передней стены (+Z) проём посередине.
    for (int layer = 0; layer < 4; ++layer) {
        const float y0 = 0.29f * static_cast<float>(layer), y1 = y0 + 0.27f;
        const glm::vec3 c = tint(kWood, 0.88f + 0.05f * static_cast<float>(layer % 2));
        addBox(m, {-kW, y0, -kD}, {kW, y1, -kD + 0.18f}, c);                      // задняя
        addBox(m, {-kW, y0, -kD + 0.18f}, {-kW + 0.18f, y1, kD - 0.18f}, c);       // боковые
        addBox(m, {kW - 0.18f, y0, -kD + 0.18f}, {kW, y1, kD - 0.18f}, c);
        if (layer >= 3) addBox(m, {-kW, y0, kD - 0.18f}, {kW, y1, kD}, c);       // передняя целиком над дверью
        else {
            addBox(m, {-kW, y0, kD - 0.18f}, {-0.3f, y1, kD}, c);
            addBox(m, {0.3f, y0, kD - 0.18f}, {kW, y1, kD}, c);
        }
    }
    addGabledRoof(m, {-kW, kH, -kD}, {kW, kH + 0.85f, kD}, 0.25f, /*ridgeAlongZ=*/true, tint(kDarkWood, 1.1f), kWood);
    addBox(m, {-0.3f, 0.0f, kD - 0.05f}, {0.3f, 0.04f, kD + 0.5f}, kLightWood);   // порог-настил
    return m;
}

MeshData buildCampfire() {
    MeshData m;
    for (int i = 0; i < 8; ++i) {   // кольцо камней
        const float a = 0.7854f * static_cast<float>(i);
        const float x = std::cos(a) * 0.62f, z = std::sin(a) * 0.62f, h = 0.16f + 0.05f * static_cast<float>(i % 3);
        addBox(m, {x - 0.13f, 0.0f, z - 0.13f}, {x + 0.13f, h, z + 0.13f}, i % 2 ? glm::vec3{0.45f, 0.45f, 0.47f} : glm::vec3{0.36f, 0.36f, 0.38f});
    }
    addBox(m, {-0.42f, 0.08f, -0.07f}, {0.42f, 0.2f, 0.07f}, kDarkWood);   // два скрещённых полена
    addBox(m, {-0.07f, 0.2f, -0.42f}, {0.07f, 0.32f, 0.42f}, kDarkWood);
    addBox(m, {-0.22f, 0.32f, -0.22f}, {0.22f, 0.62f, 0.22f}, {1.0f, 0.42f, 0.08f});    // пламя ярусами: красное -> жёлтое
    addBox(m, {-0.14f, 0.62f, -0.14f}, {0.14f, 0.92f, 0.14f}, {1.0f, 0.66f, 0.12f});
    addBox(m, {-0.07f, 0.92f, -0.07f}, {0.07f, 1.18f, 0.07f}, {1.0f, 0.88f, 0.35f});
    return m;
}

MeshData buildBanner(int tribe) {
    static const glm::vec3 kColors[] = {{0.55f, 0.55f, 0.55f}, {0.86f, 0.18f, 0.16f}, {0.16f, 0.40f, 0.92f}, {0.96f, 0.80f, 0.16f}};
    const glm::vec3 col = kColors[std::clamp(tribe, 0, 3)];
    MeshData m;
    addBox(m, {-0.03f, 0.0f, -0.03f}, {0.03f, 1.25f, 0.03f}, kDarkWood);
    addBox(m, {0.03f, 0.78f, -0.012f}, {0.5f, 1.2f, 0.012f}, col);               // полотнище
    addBox(m, {0.03f, 0.78f, -0.014f}, {0.5f, 0.84f, 0.014f}, tint(col, 0.6f));  // кайма
    return m;
}

MeshData buildCrown() {
    MeshData m;
    const glm::vec3 gold{0.95f, 0.78f, 0.15f};
    addBox(m, {-0.2f, 0.0f, -0.2f}, {0.2f, 0.09f, 0.2f}, gold);                      // обруч
    for (int i = 0; i < 4; ++i) {                                                     // зубцы по углам
        const float x = (i % 2 ? 0.17f : -0.17f), z = (i / 2 ? 0.17f : -0.17f);
        addBox(m, {x - 0.04f, 0.09f, z - 0.04f}, {x + 0.04f, 0.24f, z + 0.04f}, tint(gold, 1.05f));
    }
    return m;
}

MeshData buildAxe(int tier) {
    MeshData m;
    const float len = tier >= 2 ? 0.95f : 0.8f;
    addBox(m, {-0.03f, -0.03f, -0.1f}, {0.03f, 0.03f, len}, kLightWood);                                   // рукоять
    addBox(m, {-0.05f, -0.02f, len - 0.2f}, {0.05f, 0.1f, len - 0.02f}, tier >= 2 ? kIron : tint(kIron, 0.8f));   // обух
    addBox(m, {0.05f, -0.015f, len - 0.2f}, {tier >= 2 ? 0.26f : 0.2f, 0.09f, len - 0.04f}, tier >= 2 ? tint(kIron, 1.1f) : tint(kIron, 0.85f));   // лезвие
    if (tier >= 2) addBox(m, {-0.04f, -0.04f, 0.2f}, {0.04f, 0.04f, 0.26f}, kDarkWood);                   // обмотка
    return m;
}

MeshData buildBucket() {
    MeshData m;
    addBox(m, {-0.13f, 0.0f, -0.13f}, {0.13f, 0.28f, 0.13f}, kWood);
    addBox(m, {-0.15f, 0.08f, -0.15f}, {0.15f, 0.12f, 0.15f}, kIron);   // обруч
    addBox(m, {-0.15f, 0.2f, -0.15f}, {0.15f, 0.24f, 0.15f}, kIron);
    addBox(m, {-0.015f, 0.28f, -0.14f}, {0.015f, 0.4f, 0.14f}, kIron);  // дужка
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

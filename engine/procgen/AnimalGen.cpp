#include "procgen/AnimalGen.hpp"

#include <cmath>

namespace wsim::procgen {

namespace {

struct Look {
    float len, width, height;      // тело (длина вдоль Z, ширина, высота)
    float legLen, legR;            // ноги
    float neck, headLen, headR;    // шея (подъём головы), голова
    glm::vec3 coat, belly, dark;   // шерсть, брюхо/светлые места, тёмное (носик, копыта, кончики)
    int   ear;                     // 0 нет, 1 короткие округлые, 2 длинные заячьи, 3 острые треугольные
    int   tail;                    // 0 куцый, 1 тонкий длинный, 2 пушистый, 3 с кисточкой
    bool  antlers = false, horns = false, mane = false, spots = false, snout = false, hindHeavy = false;
};

Look lookOf(Species s) {
    switch (s) {
        case Species::Hare:     return {0.62f, 0.26f, 0.30f, 0.24f, 0.04f, 0.10f, 0.20f, 0.09f, {0.62f, 0.52f, 0.40f}, {0.90f, 0.88f, 0.82f}, {0.18f, 0.14f, 0.10f}, 2, 0, false, false, false, false, false, true};
        case Species::Deer:     return {1.15f, 0.40f, 0.52f, 0.72f, 0.05f, 0.34f, 0.34f, 0.12f, {0.58f, 0.38f, 0.22f}, {0.85f, 0.75f, 0.60f}, {0.15f, 0.11f, 0.08f}, 1, 0, true};
        case Species::Antelope: return {1.05f, 0.34f, 0.46f, 0.74f, 0.04f, 0.36f, 0.36f, 0.10f, {0.78f, 0.56f, 0.30f}, {0.95f, 0.88f, 0.72f}, {0.14f, 0.10f, 0.08f}, 3, 3, false, true};
        case Species::Tapir:    return {1.10f, 0.48f, 0.56f, 0.40f, 0.07f, 0.12f, 0.36f, 0.14f, {0.24f, 0.22f, 0.22f}, {0.80f, 0.78f, 0.72f}, {0.12f, 0.10f, 0.10f}, 1, 0, false, false, false, false, true};
        case Species::Wolf:     return {1.05f, 0.36f, 0.42f, 0.52f, 0.05f, 0.18f, 0.34f, 0.12f, {0.50f, 0.50f, 0.52f}, {0.78f, 0.76f, 0.72f}, {0.12f, 0.11f, 0.11f}, 3, 2};
        case Species::Fox:      return {0.70f, 0.26f, 0.30f, 0.30f, 0.035f, 0.10f, 0.24f, 0.08f, {0.85f, 0.42f, 0.14f}, {0.95f, 0.92f, 0.85f}, {0.14f, 0.10f, 0.08f}, 3, 2};
        case Species::Lion:     return {1.20f, 0.46f, 0.52f, 0.54f, 0.07f, 0.20f, 0.36f, 0.15f, {0.80f, 0.62f, 0.35f}, {0.90f, 0.78f, 0.58f}, {0.38f, 0.22f, 0.10f}, 1, 3, false, false, true};
        case Species::Jaguar:   return {1.00f, 0.38f, 0.44f, 0.38f, 0.06f, 0.16f, 0.34f, 0.13f, {0.82f, 0.60f, 0.20f}, {0.93f, 0.85f, 0.65f}, {0.10f, 0.08f, 0.06f}, 1, 1, false, false, false, true};
    }
    return {};
}

}  // namespace

MeshData buildAnimal(Species species) {
    const Look L = lookOf(species);
    const std::uint32_t seedBase = 700u + 31u * static_cast<std::uint32_t>(species);
    MeshData m;

    const float bodyY = L.legLen + L.height * 0.5f;  // центр тела
    const float half  = L.len * 0.5f;

    // Тело и светлое брюхо.
    addBlob(m, {0.0f, bodyY, 0.0f}, {L.width * 0.5f, L.height * 0.5f, half}, L.coat, 0.12f, seedBase + 1, 1, kMottled, 0.25f);
    addBlob(m, {0.0f, bodyY - L.height * 0.22f, 0.0f}, {L.width * 0.38f, L.height * 0.28f, half * 0.85f}, L.belly, 0.08f, seedBase + 2, 0, kMottled, 0.2f);
    // Тапир: тёмный перед и круп, светлая середина (седло).
    if (species == Species::Tapir) addBlob(m, {0.0f, bodyY + L.height * 0.12f, -half * 0.15f}, {L.width * 0.52f, L.height * 0.34f, half * 0.55f}, L.belly, 0.06f, seedBase + 3, 0, kMottled, 0.15f);

    // Ноги: сужающиеся призмы от плеча/бедра до копыта (тёмная нижняя часть).
    const float legZ = half * 0.62f, legX = L.width * 0.30f;
    for (int i = 0; i < 4; ++i) {
        const float x = (i % 2 == 0 ? -1.0f : 1.0f) * legX, z = (i < 2 ? 1.0f : -1.0f) * legZ;
        const bool hind = i >= 2;
        const float r = L.legR * 1.45f * (hind && L.hindHeavy ? 1.5f : 1.0f);   // чуть толще, чтобы с высоты камеры ноги читались
        addTaperedPrism(m, {x, L.legLen * 0.1f, z}, {x, bodyY - L.height * 0.15f, z}, r * 0.8f, r, 5, L.coat);
        addTaperedPrism(m, {x, 0.0f, z}, {x, L.legLen * 0.14f, z}, r * 0.9f, r * 0.8f, 5, L.dark);  // копыто/лапа
    }

    // Шея и голова.
    const float headY = bodyY + L.height * 0.25f + L.neck;
    const float headZ = half + L.headLen * 0.2f;
    if (L.neck > 0.15f) addTaperedPrism(m, {0.0f, bodyY + L.height * 0.1f, half * 0.65f}, {0.0f, headY - L.headR * 0.4f, headZ - L.headLen * 0.2f}, L.height * 0.22f, L.headR * 0.8f, 6, L.coat);
    addBlob(m, {0.0f, headY, headZ}, {L.headR, L.headR * 0.92f, L.headLen}, L.coat, 0.08f, seedBase + 4, 1, kMottled, 0.2f);
    // Нос/морда (тёмный кончик); у тапира -- вытянутый подвижный "хобот".
    const float snoutLen = species == Species::Tapir ? L.headLen * 0.9f : L.headLen * 0.45f;
    addBlob(m, {0.0f, headY - L.headR * 0.2f, headZ + L.headLen * 0.85f + (species == Species::Tapir ? L.headLen * 0.35f : 0.0f)},
            {L.headR * (species == Species::Tapir ? 0.55f : 0.5f), L.headR * 0.45f, snoutLen * 0.5f}, L.dark, 0.05f, seedBase + 5, 0, kPlainMaterial, 0.1f);

    // Уши.
    for (float side : {-1.0f, 1.0f}) {
        const glm::vec3 base{side * L.headR * 0.55f, headY + L.headR * 0.7f, headZ - L.headLen * 0.2f};
        switch (L.ear) {
            case 1: addTaperedPrism(m, base, base + glm::vec3(side * 0.04f, L.headR * 0.8f, -0.03f), L.headR * 0.28f, L.headR * 0.18f, 4, L.coat); break;
            case 2: addTaperedPrism(m, base, base + glm::vec3(side * 0.05f, L.headR * 3.2f, -0.1f), L.headR * 0.32f, L.headR * 0.16f, 4, L.coat); break;  // заячьи
            case 3: addTaperedPrism(m, base, base + glm::vec3(side * 0.03f, L.headR * 1.0f, -0.02f), L.headR * 0.38f, L.headR * 0.05f, 3, L.coat); break;   // острые
            default: break;
        }
    }

    // Хвост.
    const glm::vec3 tailBase{0.0f, bodyY + L.height * 0.15f, -half * 0.95f};
    switch (L.tail) {
        case 0: addBlob(m, tailBase + glm::vec3(0.0f, 0.02f, -0.04f), {0.06f, 0.06f, 0.06f}, species == Species::Hare ? glm::vec3(0.95f, 0.94f, 0.9f) : L.coat, 0.05f, seedBase + 6, 0, kPlainMaterial, 0.1f); break;
        case 1: addTaperedPrism(m, tailBase, tailBase + glm::vec3(0.0f, L.height * 0.2f, -L.len * 0.55f), L.legR * 0.7f, L.legR * 0.35f, 4, L.coat); break;
        case 2: addBlob(m, tailBase + glm::vec3(0.0f, -0.02f, -L.len * 0.25f), {L.width * 0.22f, L.width * 0.22f, L.len * 0.28f}, L.coat, 0.1f, seedBase + 6, 0, kMottled, 0.2f);
                addBlob(m, tailBase + glm::vec3(0.0f, -0.02f, -L.len * 0.5f), {L.width * 0.16f, L.width * 0.16f, L.len * 0.13f}, species == Species::Fox ? glm::vec3(0.95f, 0.93f, 0.88f) : L.dark, 0.08f, seedBase + 7, 0, kPlainMaterial, 0.1f); break;
        case 3: addTaperedPrism(m, tailBase, tailBase + glm::vec3(0.0f, -L.height * 0.15f, -L.len * 0.5f), L.legR * 0.7f, L.legR * 0.4f, 4, L.coat);
                addBlob(m, tailBase + glm::vec3(0.0f, -L.height * 0.2f, -L.len * 0.55f), {0.05f, 0.06f, 0.08f}, L.dark, 0.05f, seedBase + 7, 0, kPlainMaterial, 0.1f); break;
        default: break;
    }

    // Особенности видов.
    if (L.antlers) {  // ветвистые рога: ствол и три отростка с каждой стороны
        for (float side : {-1.0f, 1.0f}) {
            const glm::vec3 b{side * L.headR * 0.45f, headY + L.headR * 0.8f, headZ - L.headLen * 0.3f};
            const glm::vec3 tip = b + glm::vec3(side * 0.18f, 0.42f, -0.12f);
            addTaperedPrism(m, b, tip, 0.028f, 0.014f, 4, glm::vec3(0.76f, 0.68f, 0.52f));
            for (int k = 0; k < 3; ++k) {
                const float t = 0.35f + 0.22f * static_cast<float>(k);
                const glm::vec3 from = b + (tip - b) * t;
                addTaperedPrism(m, from, from + glm::vec3(side * 0.12f, 0.18f, 0.12f), 0.016f, 0.008f, 3, glm::vec3(0.76f, 0.68f, 0.52f));
            }
        }
    }
    if (L.horns) {   // антилопа: длинные прямые рога назад-вверх
        for (float side : {-1.0f, 1.0f}) {
            const glm::vec3 b{side * L.headR * 0.4f, headY + L.headR * 0.75f, headZ - L.headLen * 0.3f};
            addTaperedPrism(m, b, b + glm::vec3(side * 0.05f, 0.5f, -0.2f), 0.03f, 0.008f, 4, L.dark);
        }
    }
    if (L.mane) {    // лев: грива -- венец тёмных комков вокруг шеи и головы
        for (int k = 0; k < 8; ++k) {
            const float a = 6.2831853f * static_cast<float>(k) / 8.0f;
            addBlob(m, {std::cos(a) * L.headR * 0.95f, headY - L.headR * 0.1f + std::sin(a) * L.headR * 0.95f, headZ - L.headLen * 0.55f},
                    {L.headR * 0.5f, L.headR * 0.5f, L.headLen * 0.5f}, L.dark, 0.2f, seedBase + 10 + static_cast<std::uint32_t>(k), 0, kMottled, 0.3f);
        }
    }
    if (L.spots) {   // пятна ягуара: тёмные плоские комки по бокам и спине
        for (int k = 0; k < 14; ++k) {
            const float t = static_cast<float>(k) / 14.0f;
            const float z = -half * 0.8f + L.len * 0.8f * t;
            const float side = (k % 2 == 0) ? 1.0f : -1.0f;
            const float y = bodyY + (k % 3 == 0 ? L.height * 0.38f : L.height * 0.05f);
            const float x = (k % 3 == 0 ? side * L.width * 0.2f : side * L.width * 0.46f);
            addBlob(m, {x, y, z}, {0.045f, 0.045f, 0.06f}, L.dark, 0.1f, seedBase + 30 + static_cast<std::uint32_t>(k), 0, kPlainMaterial, 0.1f);
        }
    }
    return m;
}

}  // namespace wsim::procgen

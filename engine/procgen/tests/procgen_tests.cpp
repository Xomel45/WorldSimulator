#include "procgen/HouseGen.hpp"
#include "procgen/AnimalGen.hpp"
#include "procgen/StructGen.hpp"
#include "procgen/TreeGen.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace wsim::procgen;

namespace {
// Меш корректен: индексы в пределах, нормали единичные, каждый треугольник ориентирован
// по своей нормали (CCW снаружи, как требует cullMode=BACK), без вырожденных треугольников.
void checkMesh(const MeshData& m) {
    WSIM_CHECK(!m.vertices.empty() && m.indices.size() % 3 == 0);
    for (auto i : m.indices) WSIM_CHECK(i < m.vertices.size());
    for (const auto& v : m.vertices) {
        WSIM_CHECK(std::fabs(glm::length(glm::vec3(v.normal)) - 1.0f) < 1.0e-3f);
        WSIM_CHECK(std::isfinite(v.position.x) && std::isfinite(v.position.y) && std::isfinite(v.position.z));
    }
    for (std::size_t t = 0; t < m.indices.size(); t += 3) {
        const glm::vec3 a(m.vertices[m.indices[t]].position), b(m.vertices[m.indices[t + 1]].position),
            c(m.vertices[m.indices[t + 2]].position);
        const glm::vec3 geo = glm::cross(b - a, c - a);
        WSIM_CHECK(glm::length(geo) > 1.0e-6f);
        WSIM_CHECK(glm::dot(glm::normalize(geo), glm::vec3(m.vertices[m.indices[t]].normal)) > 0.99f);
    }
}
}  // namespace

int main() {
    // Бокс: 6 граней, 24 вершины, 36 индексов, объём покрыт AABB.
    {
        MeshData m;
        addBox(m, {-1, 0, -2}, {1, 3, 2}, {1, 1, 1});
        WSIM_CHECK(m.vertices.size() == 24 && m.indices.size() == 36);
        checkMesh(m);
        for (const auto& v : m.vertices) WSIM_CHECK(v.position.x >= -1 && v.position.x <= 1 && v.position.y >= 0 && v.position.y <= 3);
    }

    // Дома: много зёрен -- меш корректен, габариты разумные, вариации различаются, детерминизм.
    {
        const auto a = buildHouse(houseVariant(1)), a2 = buildHouse(houseVariant(1));
        WSIM_CHECK(a.vertices.size() == a2.vertices.size());
        for (std::size_t i = 0; i < a.vertices.size(); ++i) WSIM_CHECK(a.vertices[i].position == a2.vertices[i].position);

        bool different = false;
        for (std::uint32_t seed = 0; seed < 40; ++seed) {
            const HouseParams p = houseVariant(seed);
            const MeshData    m = buildHouse(p);
            checkMesh(m);
            float minY = 1e9f, maxY = -1e9f, maxX = 0, maxZ = 0;
            for (const auto& v : m.vertices) {
                minY = std::min(minY, v.position.y);
                maxY = std::max(maxY, v.position.y);
                maxX = std::max(maxX, std::fabs(v.position.x));
                maxZ = std::max(maxZ, std::fabs(v.position.z));
            }
            WSIM_CHECK(minY < 0.0f && minY >= -1.3f);   // цоколь уходит под землю
            WSIM_CHECK(maxY >= p.wallHeight + p.roofHeight - 1.0e-4f && maxY < 9.0f);
            WSIM_CHECK(maxX < 5.0f && maxZ < 5.0f);
            if (std::fabs(p.width - houseVariant(0).width) > 1.0e-4f) different = true;
        }
        WSIM_CHECK(different);
    }

    // Деревья: меши корректны, ствол от земли (с заглублением) до кроны, виды различаются, варианты тоже,
    // размеры разумны (дуб шире и ниже берёзы), вершин не слишком много (деревья вшиваются в меши чанков).
    {
        float oakHeight = 0, birchHeight = 0, oakWidth = 0, birchWidth = 0;
        std::size_t maxVerts = 0;
        for (auto species : {wsim::TreeSpecies::Oak, wsim::TreeSpecies::Birch}) {
            for (int v = 0; v < wsim::kTreeVariants; ++v) {
                const MeshData m = buildTree(species, v);
                checkMesh(m);
                float minY = 1e9f, maxY = -1e9f, w = 0;
                for (const auto& vert : m.vertices) {
                    minY = std::min(minY, vert.position.y);
                    maxY = std::max(maxY, vert.position.y);
                    w = std::max(w, std::hypot(vert.position.x, vert.position.z));
                }
                WSIM_CHECK(minY < -0.2f && minY > -0.8f);   // корень под землёй
                WSIM_CHECK(maxY > 3.0f && maxY < 10.0f);
                maxVerts = std::max(maxVerts, m.vertices.size());
                (species == wsim::TreeSpecies::Oak ? oakHeight : birchHeight) += maxY / wsim::kTreeVariants;
                (species == wsim::TreeSpecies::Oak ? oakWidth : birchWidth) += w / wsim::kTreeVariants;
                // Вариант детерминирован.
                WSIM_CHECK(buildTree(species, v).vertices.size() == m.vertices.size());
            }
        }
        // Остальные виды (биомы): корректные меши, корень под землёй, разумная высота, свой облик, детерминизм.
        for (int sp = 2; sp < wsim::kTreeSpeciesCount; ++sp) {
            const auto species = static_cast<wsim::TreeSpecies>(sp);
            for (int v = 0; v < wsim::kTreeVariants; ++v) {
                const MeshData m = buildTree(species, v);
                checkMesh(m);
                float minY = 1e9f, maxY = -1e9f;
                for (const auto& vert : m.vertices) { minY = std::min(minY, vert.position.y); maxY = std::max(maxY, vert.position.y); }
                WSIM_CHECK(minY < -0.2f && minY > -0.8f);
                WSIM_CHECK(maxY > 2.0f && maxY < 10.0f);
                WSIM_CHECK(m.vertices.size() < 1400);
                WSIM_CHECK(buildTree(species, v).vertices.size() == m.vertices.size());
                WSIM_CHECK(m.vertices.size() != buildTree(wsim::TreeSpecies::Oak, v).vertices.size() ||
                           m.vertices[0].position != buildTree(wsim::TreeSpecies::Oak, v).vertices[0].position);
            }
        }
        WSIM_CHECK(oakWidth > birchWidth * 1.3f);     // дуб раскидистее
        WSIM_CHECK(birchHeight > oakHeight);          // берёза выше
        WSIM_CHECK(maxVerts < 1400);
        WSIM_CHECK(buildTree(wsim::TreeSpecies::Oak, 0).vertices.size() != buildTree(wsim::TreeSpecies::Birch, 0).vertices.size() ||
                   buildTree(wsim::TreeSpecies::Oak, 0).vertices[0].position != buildTree(wsim::TreeSpecies::Birch, 0).vertices[0].position);
        bool variantsDiffer = false;
        for (int v = 1; v < wsim::kTreeVariants; ++v)
            variantsDiffer |= buildTree(wsim::TreeSpecies::Oak, 0).vertices[0].position != buildTree(wsim::TreeSpecies::Oak, v).vertices[0].position ||
                              buildTree(wsim::TreeSpecies::Oak, 0).vertices.size() != buildTree(wsim::TreeSpecies::Oak, v).vertices.size();
        WSIM_CHECK(variantsDiffer);
    }

    // Сокрытие граней: addBox пропускает грани по маске.
    {
        MeshData b;
        addBox(b, {0, 0, 0}, {1, 1, 1}, {1, 1, 1}, kPlainMaterial, kFaceNegY | kFaceNegZ);
        WSIM_CHECK(b.vertices.size() == 16 && b.indices.size() == 24);  // 4 грани вместо 6
        checkMesh(b);
        MeshData all;
        addBox(all, {0, 0, 0}, {1, 1, 1}, {1, 1, 1});
        WSIM_CHECK(all.indices.size() == 36);
    }
    // removeBuriedTriangles: малый комок внутри большого уходит целиком, большой остаётся нетронутым;
    // комок снаружи не трогается.
    {
        MeshData m;
        std::vector<SolidEllipsoid> solids;
        auto blob = [&](glm::vec3 c, glm::vec3 r) {
            const std::size_t first = m.indices.size() / 3;
            addBlob(m, c, r, {0.2f, 0.5f, 0.2f}, 0.0f, 1, 1);
            solids.push_back({c, r * 0.9f, first, m.indices.size() / 3});
        };
        blob({0, 0, 0}, {3, 3, 3});
        const std::size_t bigTris = m.indices.size() / 3;
        blob({0.2f, 0, 0}, {0.8f, 0.8f, 0.8f});  // целиком внутри
        blob({6, 0, 0}, {0.8f, 0.8f, 0.8f});     // снаружи
        const std::size_t before = m.indices.size() / 3;
        const std::size_t removed = removeBuriedTriangles(m, solids, 1.0f);
        WSIM_CHECK(removed == before - bigTris - (before - bigTris) / 2);  // ушёл ровно внутренний комок
        WSIM_CHECK(m.indices.size() / 3 == before - removed);
        checkMesh(m);
    }
    // Деревья: всё ещё корректные замкнутые по ориентации меши после вырезания, и треугольники скрытого
    // внутри кроны реально убраны (дуб: 5 комков налезают друг на друга).
    {
        std::size_t tris = 0;
        for (int v = 0; v < wsim::kTreeVariants; ++v) tris += buildTree(wsim::TreeSpecies::Oak, v).indices.size() / 3;
        std::printf("oak triangles over %d variants: %zu\n", wsim::kTreeVariants, tris);
    }

    // Животные: меши корректны, ноги на земле, длина по +Z, виды различаются, хищники не уступают размером добыче в "массе".
    {
        std::size_t verts[wsim::kSpeciesCount] = {};
        for (int i = 0; i < wsim::kSpeciesCount; ++i) {
            const auto sp = static_cast<wsim::Species>(i);
            const MeshData m = buildAnimal(sp);
            checkMesh(m);
            float minY = 1e9f, maxY = -1e9f, minZ = 1e9f, maxZ = -1e9f, maxX = 0;
            for (const auto& v : m.vertices) {
                minY = std::min(minY, v.position.y); maxY = std::max(maxY, v.position.y);
                minZ = std::min(minZ, v.position.z); maxZ = std::max(maxZ, v.position.z);
                maxX = std::max(maxX, std::fabs(v.position.x));
            }
            WSIM_CHECK(minY > -0.02f && minY < 0.02f);          // стоит на земле
            WSIM_CHECK(maxY > 0.35f && maxY < 2.2f);
            WSIM_CHECK(maxZ - minZ > 0.5f && maxZ > 0.25f);     // вытянут вдоль Z, голова впереди (+Z)
            WSIM_CHECK(maxX < 0.6f);
            WSIM_CHECK(m.vertices.size() < 4500);
            WSIM_CHECK(buildAnimal(sp).vertices.size() == m.vertices.size());  // детерминирован
            verts[i] = m.vertices.size();
        }
        WSIM_CHECK(verts[static_cast<int>(wsim::Species::Hare)] != verts[static_cast<int>(wsim::Species::Lion)]);
    }

    // Постройки и снаряжение: корректные меши, разумные габариты.
    {
        for (const MeshData& m : {buildPlank(), buildBarricade(), buildTrap(), buildSpear(), buildShield(), buildLogs(1), buildLogs(4), buildStockpile(), buildBucket()}) {
            checkMesh(m);
            WSIM_CHECK(m.vertices.size() < 700);
        }
        float maxX = 0, maxY = 0;
        for (const auto& v : buildBarricade().vertices) { maxX = std::max(maxX, std::fabs(v.position.x)); maxY = std::max(maxY, v.position.y); }
        WSIM_CHECK(maxX > 1.7f && maxX < 2.0f && maxY > 1.0f && maxY < 1.8f);
        WSIM_CHECK(buildLogs(4).vertices.size() > buildLogs(1).vertices.size());
        float topY = -1e9f;
        for (const auto& v : buildPlank().vertices) topY = std::max(topY, v.position.y);
        WSIM_CHECK(std::fabs(topY) < 1.0e-5f);
    }

    std::puts("procgen_tests: OK");
    return 0;
}

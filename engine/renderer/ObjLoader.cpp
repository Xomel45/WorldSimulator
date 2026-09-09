#include "renderer/ObjLoader.hpp"

#include "core/Logger.hpp"

#include <glm/glm.hpp>

#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace wsim {

namespace {

// "1/2/3" или "1//3" → индексы (v, vn); OBJ индексирует с 1, отрицательные — с конца.
struct FaceRef {
    int v  = 0;
    int vn = 0;
};

FaceRef parseFaceRef(std::string_view token) {
    FaceRef ref;
    const size_t s1 = token.find('/');
    std::from_chars(token.data(), token.data() + token.size(), ref.v);
    if (s1 != std::string_view::npos) {
        const size_t s2 = token.find('/', s1 + 1);
        if (s2 != std::string_view::npos)
            std::from_chars(token.data() + s2 + 1, token.data() + token.size(), ref.vn);
    }
    return ref;
}

// MTL: имя материала → диффузный цвет Kd
std::unordered_map<std::string, glm::vec3> loadMtl(const std::string& path) {
    std::unordered_map<std::string, glm::vec3> mats;
    std::ifstream file(path);
    if (!file) return mats;  // MTL опционален

    std::string line, current;
    while (std::getline(file, line)) {
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "newmtl") {
            ss >> current;
        } else if (tag == "Kd" && !current.empty()) {
            glm::vec3 kd{};
            ss >> kd.r >> kd.g >> kd.b;
            mats[current] = kd;
        }
    }
    return mats;
}

}  // namespace

std::pair<std::vector<Vertex>, std::vector<uint32_t>> loadObj(const std::string& path) {
    std::ifstream file(path);
    if (!file)
        throw std::runtime_error("loadObj: не удалось открыть " + path);

    const std::string dir = path.substr(0, path.find_last_of('/') + 1);

    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::unordered_map<std::string, glm::vec3> materials;
    glm::vec3 kd{0.6f, 0.6f, 0.6f};  // fallback, если usemtl не встретился

    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    // Дедупликация: (v, vn, материал) → индекс. Ключ пакуется в строку — просто и достаточно.
    std::unordered_map<std::string, uint32_t> unique;

    std::string line;
    while (std::getline(file, line)) {
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;

        if (tag == "v") {
            glm::vec3 p{};
            ss >> p.x >> p.y >> p.z;
            positions.push_back(p);
        } else if (tag == "vn") {
            glm::vec3 n{};
            ss >> n.x >> n.y >> n.z;
            normals.push_back(n);
        } else if (tag == "mtllib") {
            std::string mtl;
            ss >> mtl;
            materials = loadMtl(dir + mtl);
        } else if (tag == "usemtl") {
            std::string name;
            ss >> name;
            if (auto it = materials.find(name); it != materials.end()) kd = it->second;
        } else if (tag == "f") {
            // Треугольники гарантированы export_triangulated_mesh=True
            std::string t0, t1, t2;
            ss >> t0 >> t1 >> t2;
            for (const auto& tok : {t0, t1, t2}) {
                const FaceRef ref = parseFaceRef(tok);
                const int vi  = ref.v  > 0 ? ref.v - 1  : (int)positions.size() + ref.v;
                const int ni  = ref.vn > 0 ? ref.vn - 1 : (int)normals.size()   + ref.vn;

                std::string key = tok + "|" + std::to_string(kd.r) + "," +
                                  std::to_string(kd.g) + "," + std::to_string(kd.b);
                if (auto it = unique.find(key); it != unique.end()) {
                    indices.push_back(it->second);
                    continue;
                }

                Vertex v{};
                v.position = glm::vec4(positions.at((size_t)vi), 0.f);
                v.normal   = ref.vn != 0 ? glm::vec4(normals.at((size_t)ni), 0.f)
                                         : glm::vec4(0.f, 1.f, 0.f, 0.f);
                v.color    = glm::vec4(kd, 1.f);

                const auto idx = (uint32_t)vertices.size();
                vertices.push_back(v);
                unique.emplace(std::move(key), idx);
                indices.push_back(idx);
            }
        }
    }

    if (vertices.empty() || indices.empty())
        throw std::runtime_error("loadObj: пустой меш в " + path);

    Logger::info("OBJ {}: {} verts, {} tris", path, vertices.size(), indices.size() / 3);
    return {std::move(vertices), std::move(indices)};
}

}  // namespace wsim

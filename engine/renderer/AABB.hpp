#pragma once

#include <glm/glm.hpp>

#include <cfloat>

namespace wsim {

// Axis-Aligned Bounding Box в локальном или мировом пространстве.
struct AABB {
    glm::vec3 min{ FLT_MAX,  FLT_MAX,  FLT_MAX};
    glm::vec3 max{-FLT_MAX, -FLT_MAX, -FLT_MAX};

    [[nodiscard]] bool isValid() const noexcept { return min.x <= max.x; }

    void expand(glm::vec3 p) noexcept {
        min = glm::min(min, p);
        max = glm::max(max, p);
    }

    // Трансформировать в новое пространство: перебираем все 8 углов,
    // применяем матрицу и пересчитываем min/max.
    [[nodiscard]] AABB transformed(const glm::mat4& m) const noexcept {
        AABB result;
        const glm::vec3 corners[8] = {
            {min.x, min.y, min.z}, {max.x, min.y, min.z},
            {min.x, max.y, min.z}, {max.x, max.y, min.z},
            {min.x, min.y, max.z}, {max.x, min.y, max.z},
            {min.x, max.y, max.z}, {max.x, max.y, max.z},
        };
        for (const auto& c : corners)
            result.expand(glm::vec3(m * glm::vec4(c, 1.0f)));
        return result;
    }
};

}  // namespace wsim

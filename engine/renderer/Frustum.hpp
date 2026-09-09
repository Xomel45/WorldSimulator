#pragma once

#include "renderer/AABB.hpp"

#include <glm/glm.hpp>

namespace wsim {

// Frustum из 6 плоскостей в мировом пространстве.
// Плоскость (nx, ny, nz, d): точка P внутри если dot(n, P) + d >= 0.
struct Frustum {
    glm::vec4 planes[6];
    // [0] Left  [1] Right  [2] Bottom  [3] Top  [4] Near  [5] Far

    // Извлечь из View-Projection матрицы (Gribb-Hartmann).
    // Требует GLM_FORCE_DEPTH_ZERO_TO_ONE (Vulkan: near z=0, far z=1).
    [[nodiscard]] static Frustum fromMatrix(const glm::mat4& vp) noexcept {
        // Строки VP-матрицы: row_i = (vp[0][i], vp[1][i], vp[2][i], vp[3][i])
        auto row = [&](int r) -> glm::vec4 {
            return {vp[0][r], vp[1][r], vp[2][r], vp[3][r]};
        };
        const glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);

        Frustum f;
        f.planes[0] = r3 + r0;  // Left:   x+w >= 0
        f.planes[1] = r3 - r0;  // Right:  w-x >= 0
        f.planes[2] = r3 + r1;  // Bottom: y+w >= 0
        f.planes[3] = r3 - r1;  // Top:    w-y >= 0
        f.planes[4] = r2;        // Near:   z >= 0   (Vulkan 0..1 depth)
        f.planes[5] = r3 - r2;  // Far:    w-z >= 0
        return f;
    }

    // Проверить пересечение с AABB (world space).
    // "Positive vertex" тест: если для любой плоскости самый выдвинутый угол
    // AABB находится за ней — объект полностью за пределами фрустума.
    [[nodiscard]] bool intersects(const AABB& aabb) const noexcept {
        if (!aabb.isValid()) return false;

        for (const auto& p : planes) {
            const glm::vec3 n(p);
            // Угол, максимально выдвинутый в сторону нормали плоскости
            const glm::vec3 pv{
                n.x >= 0.f ? aabb.max.x : aabb.min.x,
                n.y >= 0.f ? aabb.max.y : aabb.min.y,
                n.z >= 0.f ? aabb.max.z : aabb.min.z,
            };
            if (glm::dot(n, pv) + p.w < 0.f) return false;  // полностью снаружи
        }
        return true;
    }
};

}  // namespace wsim

#pragma once

#include "renderer/AABB.hpp"
#include "renderer/Mesh.hpp"
#include "renderer/Transform.hpp"

#include <cstdint>
#include <memory>

namespace wsim {

// Объект сцены: меш (shared) + трансформ.
// Несколько объектов могут ссылаться на один и тот же меш.
struct SceneObject {
    std::shared_ptr<Mesh> mesh;
    Transform             transform;
    bool                  solid = false;  // участвует в коллизии с игроком
    // Для векторов движения (temporal-апскейлер): на сколько объект сместился в мире с прошлого
    // нарисованного кадра и по чему его узнать между кадрами (0 -- без отслеживания).
    glm::vec3             motion{0.0f};
    std::uint64_t         trackId = 0;

    [[nodiscard]] glm::mat4 modelMatrix() const noexcept {
        return transform.matrix();
    }

    // AABB объекта в мировом пространстве.
    [[nodiscard]] AABB worldAABB() const noexcept {
        if (!mesh) return AABB{};
        return mesh->localAABB.transformed(transform.matrix());
    }
};

}  // namespace wsim

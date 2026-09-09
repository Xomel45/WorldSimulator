#pragma once

#include "renderer/AABB.hpp"
#include "renderer/Mesh.hpp"
#include "renderer/Transform.hpp"

#include <memory>

namespace wsim {

// Объект сцены: меш (shared) + трансформ.
// Несколько объектов могут ссылаться на один и тот же меш.
struct SceneObject {
    std::shared_ptr<Mesh> mesh;
    Transform             transform;
    bool                  solid = false;  // участвует в коллизии с игроком

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

#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace wsim {

// Трансформ объекта: позиция / поворот (Euler, градусы) / масштаб.
// Порядок вращения: Y (yaw) → X (pitch) → Z (roll).
struct Transform {
    glm::vec3 position = {0.f, 0.f, 0.f};
    glm::vec3 rotation = {0.f, 0.f, 0.f};  // degrees: x=pitch, y=yaw, z=roll
    glm::vec3 scale    = {1.f, 1.f, 1.f};

    [[nodiscard]] glm::mat4 matrix() const noexcept {
        glm::mat4 m(1.f);
        m = glm::translate(m, position);
        m = glm::rotate(m, glm::radians(rotation.y), {0.f, 1.f, 0.f});
        m = glm::rotate(m, glm::radians(rotation.x), {1.f, 0.f, 0.f});
        m = glm::rotate(m, glm::radians(rotation.z), {0.f, 0.f, 1.f});
        m = glm::scale(m, scale);
        return m;
    }
};

}  // namespace wsim

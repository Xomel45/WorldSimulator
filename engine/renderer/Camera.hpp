#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace wsim {

// UBO layout — std140, передаётся в scene.vert (set=0, binding=0)
struct CameraData {
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 position;  // xyz = позиция камеры, w = 0
};

// God-камера: вращается вокруг подвижной точки (target) в плоскости земли.
// pan() двигает target, orbit() вращает вокруг неё, zoom() меняет дистанцию.
class OrbitCamera {
public:
    OrbitCamera() = default;

    void setAspect(float aspect) noexcept { m_aspect = aspect; }

    // Двигает target по плоской (yaw-only, без учёта pitch) системе координат
    // камеры — так панорамирование остаётся на уровне земли при любом наклоне.
    // dForward — вдоль направления взгляда камеры (проекция на XZ),
    // dRight — вбок от него.
    void pan(float dForward, float dRight) noexcept {
        const float yawRad = glm::radians(m_yaw);
        const glm::vec3 flatForward{cos(yawRad), 0.f, sin(yawRad)};
        const glm::vec3 flatRight = glm::normalize(glm::cross(flatForward, kUp));
        m_target += flatForward * dForward + flatRight * dRight;
    }

    // Вращение вокруг target: dyaw вокруг Y, dpitch вокруг X (в градусах).
    void orbit(float dyaw, float dpitch) noexcept {
        m_yaw   += dyaw;
        m_pitch  = glm::clamp(m_pitch + dpitch, kMinPitch, kMaxPitch);
    }

    // Изменение дистанции до target (отрицательное — приближение).
    void zoom(float dDistance) noexcept {
        m_distance = glm::clamp(m_distance + dDistance, kMinDistance, kMaxDistance);
    }

    [[nodiscard]] glm::vec3 target() const noexcept { return m_target; }
    [[nodiscard]] float     yaw() const noexcept { return m_yaw; }
    [[nodiscard]] float     pitch() const noexcept { return m_pitch; }
    [[nodiscard]] float     distance() const noexcept { return m_distance; }

    // Направление от камеры к target (нормализованный вектор).
    [[nodiscard]] glm::vec3 front() const noexcept {
        return glm::normalize(glm::vec3{
            cos(glm::radians(m_yaw))  * cos(glm::radians(m_pitch)),
            sin(glm::radians(m_pitch)),
            sin(glm::radians(m_yaw))  * cos(glm::radians(m_pitch)),
        });
    }

    [[nodiscard]] glm::vec3 position() const noexcept {
        return m_target - front() * m_distance;
    }

    [[nodiscard]] CameraData getData() const noexcept {
        const glm::vec3 pos = position();
        auto view = glm::lookAt(pos, m_target, kUp);

        // glm::perspective даёт OpenGL-NDC (Y+ вверх).
        // Умножаем proj[1][1] на -1, чтобы получить Vulkan-NDC (Y+ вниз).
        auto proj = glm::perspective(glm::radians(m_fov), m_aspect, m_near, m_far);
        proj[1][1] *= -1.0f;

        return {view, proj, glm::vec4(pos, 0.0f)};
    }

private:
    static constexpr glm::vec3 kUp{0.0f, 1.0f, 0.0f};
    static constexpr float     kMinPitch    = -85.0f;
    static constexpr float     kMaxPitch    = -20.0f;
    static constexpr float     kMinDistance = 4.0f;
    static constexpr float     kMaxDistance = 200.0f;

    glm::vec3 m_target   = {0.0f, 0.0f, 0.0f};
    float     m_yaw      = -90.0f;
    float     m_pitch    = -45.0f;
    float     m_distance = 25.0f;
    float     m_fov      = 50.0f;
    float     m_near     = 0.1f;
    float     m_far      = 1000.0f;
    float     m_aspect   = 1920.0f / 1080.0f;
};

}  // namespace wsim

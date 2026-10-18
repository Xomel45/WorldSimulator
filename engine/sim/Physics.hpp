#pragma once

#include "sim/Terrain.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <vector>

namespace wsim {

// Шар-тело (валун): гравитация, столкновение с рельефом (карта высот Terrain) и
// друг с другом, трение качения, сон. Идея и параметры (сон по скорости,
// позиционная коррекция с допуском, события ударов для звука/эффектов)
// взяты из PhysicsWorld Podjezd, но солвер другой: там AABB-коробки против
// статичных коробок, здесь шары против поверхности-функции высоты -- так
// валуны катятся по склонам, а не скользят кубом.
struct RigidBody {
    glm::vec3 pos{0.0f};
    glm::vec3 vel{0.0f};
    glm::quat orient{1.0f, 0.0f, 0.0f, 0.0f};  // только для отрисовки: накапливается качением
    float     radius      = 1.0f;
    float     invMass     = 1.0f;
    float     restitution = 0.30f;
    bool      asleep      = false;
    float     sleepTimer  = 0.0f;   // сколько секунд почти не двигается
    float     asleepFor   = 0.0f;   // сколько секунд уже спит (для уборки)
};

// Удар о рельеф сильнее порога: позиция, импульс (масса * скорость удара), радиус тела.
struct ImpactEvent {
    glm::vec3 pos;
    float     impulse;
    float     radius;
};

class PhysicsWorld {
public:
    static constexpr float       kGravity      = 18.0f;
    static constexpr std::size_t kMaxBodies    = 48;     // старые убираются, когда новых больше
    static constexpr float       kImpactSpeed  = 6.0f;   // медленнее -- не удар, а касание
    static constexpr float       kSleepSpeed   = 0.25f;
    static constexpr float       kSleepDelay   = 0.8f;
    static constexpr float       kRemoveAfterAsleep = 90.0f;  // спящий валун исчезает через столько секунд

    // Новый шар; density задаёт массу: radius^3 * density. Возвращает индекс.
    // Индексы и ссылки на тела живут до следующего step()/addSphere() -- не хранить.
    int addSphere(const glm::vec3& pos, float radius, float density = 1.0f, const glm::vec3& vel = glm::vec3(0.0f));

    // Шаг; terrain -- поверхность и вода. Накапливает impacts() (очищается каждый шаг).
    void step(float dt, const Terrain& terrain);

    void clear() { m_bodies.clear(); m_impacts.clear(); }
    void setBodies(std::vector<RigidBody> bodies) { m_bodies = std::move(bodies); m_impacts.clear(); }  // загрузка сохранения

    [[nodiscard]] const std::vector<RigidBody>&   bodies() const noexcept { return m_bodies; }
    [[nodiscard]] const std::vector<ImpactEvent>& impacts() const noexcept { return m_impacts; }

private:
    void collideTerrain(RigidBody& b, const Terrain& terrain, float dt);
    void collidePair(RigidBody& a, RigidBody& b);

    std::vector<RigidBody>   m_bodies;
    std::vector<ImpactEvent> m_impacts;
};

}  // namespace wsim

#include "renderer/Camera.hpp"
#include "renderer/Shadows.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>

using namespace wsim;

namespace {
// Точка в мире на глубине вида `depth` вдоль луча через NDC (nx, ny).
glm::vec3 viewPoint(const CameraData& cam, float nx, float ny, float depth) {
    const glm::mat4 invVP = glm::inverse(cam.proj * cam.view);
    const glm::vec4 a = invVP * glm::vec4(nx, ny, 0.25f, 1.0f), b = invVP * glm::vec4(nx, ny, 0.75f, 1.0f);
    const glm::vec3 origin = glm::vec3(cam.position);
    const glm::vec3 dir    = glm::normalize(glm::vec3(b / b.w) - glm::vec3(a / a.w));
    const glm::vec3 fwd    = glm::normalize(glm::vec3(glm::inverse(cam.view)[2]) * -1.0f);
    return origin + dir * (depth / glm::dot(dir, fwd));  // depth -- вдоль оси камеры, не по лучу
}
}  // namespace

int main() {
    const glm::vec3 toSun = glm::normalize(glm::vec3(0.4f, 1.0f, 0.3f));
    constexpr int   kSize = 2048;

    OrbitCamera camera;
    for (float distance : {10.0f, 60.0f, 200.0f}) {
        for (float yaw : {0.0f, 77.0f, 200.0f}) {
            OrbitCamera c;
            c.zoom(distance - c.distance());
            c.orbit(yaw, 0.0f);
            const CameraData cam = c.getData();
            const CascadeSet cs  = computeCascades(cam.view, cam.proj, c.nearPlane(), toSun, kSize);

            // Всё, что камера видит в пределах каскада, попадает в его карту (клип в [-1,1], глубина в [0,1]).
            float sliceNear = c.nearPlane();
            for (int i = 0; i < kShadowCascades; ++i) {
                const float sliceFar = cs.splitFar[static_cast<std::size_t>(i)];
                WSIM_CHECK(sliceFar > sliceNear);
                for (float nx : {-1.0f, 0.0f, 1.0f})
                    for (float ny : {-1.0f, 0.0f, 1.0f})
                        for (float d : {sliceNear + 0.01f, 0.5f * (sliceNear + sliceFar), sliceFar - 0.01f}) {
                            const glm::vec3 p  = viewPoint(cam, nx, ny, d);
                            const glm::vec4 lc = cs.viewProj[static_cast<std::size_t>(i)] * glm::vec4(p, 1.0f);
                            const glm::vec3 n  = glm::vec3(lc) / lc.w;
                            WSIM_CHECK(std::fabs(n.x) <= 1.001f && std::fabs(n.y) <= 1.001f);
                            WSIM_CHECK(n.z >= 0.0f && n.z <= 1.0f);

                            // Тело ближе к солнцу, над точкой, лежит между ней и источником света (глубина меньше),
                            // и всё ещё внутри карты: именно оно отбрасывает тень.
                            const glm::vec4 lh = cs.viewProj[static_cast<std::size_t>(i)] * glm::vec4(p + toSun * 100.0f, 1.0f);
                            const glm::vec3 nh = glm::vec3(lh) / lh.w;
                            WSIM_CHECK(nh.z < n.z && nh.z >= 0.0f);
                        }
                sliceNear = sliceFar;
            }

            // Стабильность: сдвиг камеры на целое число текселей каскада не меняет сетку теней (привязка к текселям).
            OrbitCamera moved = c;
            moved.pan(0.0f, 0.0f);
            const CascadeSet cs2 = computeCascades(moved.getData().view, moved.getData().proj, c.nearPlane(), toSun, kSize);
            for (int i = 0; i < kShadowCascades; ++i)
                WSIM_CHECK(cs.texelWorld[static_cast<std::size_t>(i)] == cs2.texelWorld[static_cast<std::size_t>(i)]);
        }
    }

    // Каскады растут: дальше шире и грубее.
    const CameraData cam = camera.getData();
    const CascadeSet cs  = computeCascades(cam.view, cam.proj, camera.nearPlane(), toSun, kSize);
    for (int i = 1; i < kShadowCascades; ++i)
        WSIM_CHECK(cs.texelWorld[static_cast<std::size_t>(i)] > cs.texelWorld[static_cast<std::size_t>(i - 1)]);

    std::puts("shadows_tests: OK");
    return 0;
}

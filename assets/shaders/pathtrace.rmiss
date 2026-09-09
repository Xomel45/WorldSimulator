#version 460
#extension GL_EXT_ray_tracing : require

struct RayPayload {
    vec3  radiance;
    vec3  albedo;
    vec3  hitPos;
    vec3  hitNormal;
    float hitT;
};
layout(location = 0) rayPayloadInEXT RayPayload pay;

void main() {
    // Тёмное холодное «небо» подъезда (лунный отлив), throughput дальше не идёт
    pay.radiance = vec3(0.006, 0.008, 0.012);
    pay.hitT     = -1.0;
}

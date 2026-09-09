#version 460
#extension GL_EXT_ray_tracing          : require
#extension GL_EXT_ray_query            : require
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference2    : require

// Шейдинг точки попадания первичного/отскочившего луча: достаём геометрию меша,
// интерполируем нормаль+albedo, считаем прямой свет (Blinn-Phong + жёсткая тень).

layout(set = 0, binding = 0) uniform CameraUBO { mat4 view; mat4 proj; vec4 camPos; };

#include "lighting.glsl"   // LightsUBO @ set0/binding1, shade(), forward shadowFactor()

layout(set = 0, binding = 2) uniform accelerationStructureEXT topLevelAS;

struct RtVertex { vec4 pos; vec4 nrm; vec4 col; };
layout(buffer_reference, std430) readonly buffer Vertices { RtVertex v[]; };
layout(buffer_reference, std430) readonly buffer Indices  { uint     i[]; };
struct Geo { Vertices verts; Indices inds; };
layout(set = 0, binding = 4, std430) readonly buffer GeoTable { Geo geos[]; };

struct RayPayload {
    vec3  radiance;
    vec3  albedo;
    vec3  hitPos;
    vec3  hitNormal;
    float hitT;
};
layout(location = 0) rayPayloadInEXT RayPayload pay;
hitAttributeEXT vec2 attribs;  // барицентрики треугольника

// Жёсткая тень (1 луч) через ray_query — на отскоках дороже не нужно.
float shadowFactor(vec3 origin, vec3 dir, float maxDist) {
    if (maxDist <= 0.0) return 1.0;
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, topLevelAS,
        gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT,
        0xFF, origin, 0.015, dir, maxDist);
    rayQueryProceedEXT(rq);
    return rayQueryGetIntersectionTypeEXT(rq, true) ==
           gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}

void main() {
    Geo  g  = geos[gl_InstanceCustomIndexEXT];
    uint i0 = g.inds.i[gl_PrimitiveID * 3 + 0];
    uint i1 = g.inds.i[gl_PrimitiveID * 3 + 1];
    uint i2 = g.inds.i[gl_PrimitiveID * 3 + 2];
    RtVertex v0 = g.verts.v[i0];
    RtVertex v1 = g.verts.v[i1];
    RtVertex v2 = g.verts.v[i2];

    vec3 w      = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
    vec3 localN = v0.nrm.xyz * w.x + v1.nrm.xyz * w.y + v2.nrm.xyz * w.z;
    vec3 albedo = v0.col.rgb * w.x + v1.col.rgb * w.y + v2.col.rgb * w.z;

    vec3 dir = gl_WorldRayDirectionEXT;
    vec3 N   = normalize(mat3(gl_ObjectToWorldEXT) * localN);
    if (dot(N, -dir) < 0.0) N = -N;                // двусторонняя нормаль
    vec3 P = gl_WorldRayOriginEXT + dir * gl_HitTEXT;

    pay.radiance  = shade(P, N, -dir, albedo, 1.0);
    pay.albedo    = albedo;
    pay.hitPos    = P;
    pay.hitNormal = N;
    pay.hitT      = gl_HitTEXT;
}

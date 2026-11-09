// GPU particles (rhi::Device::simulate_particles / draw_gpu_particles; Niagara's GPU Compute Sim), shared by
// D3D12 (embedded as text, compiled at startup) and Vulkan (SPIR-V at build time); OpenGL keeps a GLSL mirror.
// Each particle is 3 float4s: position + age, velocity + lifetime, rotation + spin (degrees, degrees a second).
// Dead when age >= lifetime, so a zeroed buffer is all dead. PARTICLE_SIM: the compute pass (gpu_particle_sim);
// PARTICLE_DRAW: the sprites' vertex shader (gpu_particle_vs), 6 vertices per particle, drawn with the particle pixel shader.
#ifdef __spirv__
#define VK_BIND(b, s) [[vk::binding(b, s)]]
#else
#define VK_BIND(b, s)
#endif
struct Params {  // rhi::GpuParticles
    float4 spawn_rows[3];
    float4 draw_rows[3];
    uint4 spawn;       // first slot, count, capacity, seed
    float4 shape;      // type (0 point, 1 sphere, 2 box), size xyz
    float4 direction;  // xyz (unit), cos(spread)
    float4 speed_life; // speed min, max; lifetime min, max
    float4 force;      // acceleration xyz, drag
    float4 step;       // dt, floor height, bounce, friction
    float4 flags;      // floor collision, kill on hit, spin
    float4 size;       // start, end, size key count, color key count
    float4 color_start;
    float4 color_end;
    float4 size_keys[4];    // (time, value) pairs
    float4 color_times[2];
    float4 color_keys[8];
    float4 right;
    float4 up;
};

#ifdef PARTICLE_SIM
VK_BIND(0, 0) cbuffer ParticleParams : register(b0) { Params p; };
VK_BIND(1, 0) RWStructuredBuffer<float4> particles : register(u0);
float rand(inout uint s) {  // PCG hash: 0..1
    s = s * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    w = (w >> 22u) ^ w;
    return (float)(w >> 8) / 16777216.0;
}
float3 rows_point(float4 r[3], float3 v) { return float3(dot(r[0], float4(v, 1)), dot(r[1], float4(v, 1)), dot(r[2], float4(v, 1))); }
float3 rows_vector(float4 r[3], float3 v) { return float3(dot(r[0].xyz, v), dot(r[1].xyz, v), dot(r[2].xyz, v)); }
[numthreads(64, 1, 1)]
void gpu_particle_sim(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x, capacity = p.spawn.z;
    if (i >= capacity) return;
    float4 a = particles[i * 3], b = particles[i * 3 + 1], c = particles[i * 3 + 2];
    if ((i + capacity - p.spawn.x) % capacity < p.spawn.y) {  // born this step (as ParticleInstance::spawn)
        uint s = (i * 9781u + 1u) ^ (p.spawn.w * 6271u);
        float3 local = 0;
        if (p.shape.x > 1.5) {
            local = (float3(rand(s), rand(s), rand(s)) * 2.0 - 1.0) * p.shape.yzw;
        } else if (p.shape.x > 0.5) {  // uniform in the sphere's volume
            const float z = rand(s) * 2.0 - 1.0, phi = rand(s) * 6.2831853, r = pow(rand(s), 1.0 / 3.0);
            local = float3(sqrt(1.0 - z * z) * cos(phi), sqrt(1.0 - z * z) * sin(phi), z) * r * p.shape.y;
        }
        const float3 axis = p.direction.xyz;
        const float cos_t = 1.0 - rand(s) * (1.0 - p.direction.w), sin_t = sqrt(max(0.0, 1.0 - cos_t * cos_t)), phi = rand(s) * 6.2831853;
        const float3 u = normalize(cross(abs(axis.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0), axis)), v = cross(axis, u);
        const float3 dir = axis * cos_t + u * (sin_t * cos(phi)) + v * (sin_t * sin(phi));
        const float speed = lerp(p.speed_life.x, p.speed_life.y, rand(s));
        a = float4(rows_point(p.spawn_rows, local), 0.0);
        b = float4(rows_vector(p.spawn_rows, dir * speed), lerp(p.speed_life.z, p.speed_life.w, rand(s)));
        c = float4(rand(s) * 360.0, (rand(s) * 2.0 - 1.0) * p.flags.z, 0.0, 0.0);
    } else if (a.w < b.w) {  // alive: as ParticleInstance::update
        const float dt = p.step.x;
        a.w += dt;
        b.xyz = (b.xyz + p.force.xyz * dt) * max(0.0, 1.0 - p.force.w * dt);
        const float from = a.z;
        a.xyz += b.xyz * dt;
        c.x += c.y * dt;
        if (p.flags.x > 0.5 && from >= p.step.y && a.z < p.step.y) {  // the floor
            if (p.flags.y > 0.5) a.w = b.w;
            a.z = p.step.y + 0.01;
            b.xy *= 1.0 - p.step.w;
            b.z = -b.z * p.step.z;
        }
    } else {
        return;
    }
    particles[i * 3] = a;
    particles[i * 3 + 1] = b;
    particles[i * 3 + 2] = c;
}
#endif

#ifdef PARTICLE_DRAW
#ifdef __spirv__
struct Push { float4x4 view_proj; float4 mode; float4 soft; };  // as particle.hlsl's
[[vk::push_constant]] Push pc;
#define view_proj pc.view_proj
#else
cbuffer Draw : register(b0) { float4x4 view_proj; float4 mode; float4 soft; };  // as the particle pixel shader's
#endif
VK_BIND(0, 2) cbuffer ParticleParams : register(b2) { Params p; };
VK_BIND(1, 2) StructuredBuffer<float4> particles : register(t2);
struct VsOut { float4 pos : SV_Position; float4 color : COLOR; float2 uv : TEXCOORD; };
// The over-life keys (particles::size_at / color_at): the latest key at or before t and the earliest after it.
float size_at(float t) {
    const uint n = (uint)p.size.z;
    if (n == 0) return lerp(p.size.x, p.size.y, t);
    int before = -1, after = -1;
    float tb = 0, ta = 0, vb = 0, va = 0;
    for (uint k = 0; k < n; ++k) {
        const float2 key = (k & 1u) ? p.size_keys[k >> 1].zw : p.size_keys[k >> 1].xy;
        if (key.x <= t && (before < 0 || key.x >= tb)) { before = (int)k; tb = key.x; vb = key.y; }
        if (key.x > t && (after < 0 || key.x < ta)) { after = (int)k; ta = key.x; va = key.y; }
    }
    if (before < 0) return p.size.x * va;
    if (after < 0) return p.size.x * vb;
    return p.size.x * lerp(vb, va, (t - tb) / (ta - tb));
}
float4 color_at(float t) {
    const uint n = (uint)p.size.w;
    if (n == 0) return lerp(p.color_start, p.color_end, t);
    int before = -1, after = -1;
    float tb = 0, ta = 0;
    for (uint k = 0; k < n; ++k) {
        const float key = p.color_times[k >> 2][k & 3u];
        if (key <= t && (before < 0 || key >= tb)) { before = (int)k; tb = key; }
        if (key > t && (after < 0 || key < ta)) { after = (int)k; ta = key; }
    }
    if (before < 0) return p.color_keys[after];
    if (after < 0) return p.color_keys[before];
    return lerp(p.color_keys[before], p.color_keys[after], (t - tb) / (ta - tb));
}
VsOut gpu_particle_vs(uint vertex : SV_VertexID) {
    VsOut o;
    const uint i = vertex / 6u, k = vertex % 6u;
    const float4 a = particles[i * 3], b = particles[i * 3 + 1], c = particles[i * 3 + 2];
    o.color = 0;
    o.uv = 0;
    o.pos = float4(2.0, 2.0, 2.0, 1.0);  // dead: outside the view
    if (a.w >= b.w) return o;
    const float t = a.w / b.w;
    const float2 corner = float2(k == 1u || k == 2u || k == 4u ? 1.0 : -1.0, k == 2u || k == 4u || k == 5u ? 1.0 : -1.0);
    const float half_size = 0.5 * size_at(t), r = radians(c.x), cs = cos(r), sn = sin(r);
    const float3 x = (p.right.xyz * cs + p.up.xyz * sn) * half_size, y = (p.up.xyz * cs - p.right.xyz * sn) * half_size;
    const float3 centre = float3(dot(p.draw_rows[0], float4(a.xyz, 1)), dot(p.draw_rows[1], float4(a.xyz, 1)), dot(p.draw_rows[2], float4(a.xyz, 1)));
    o.pos = mul(view_proj, float4(centre + x * corner.x + y * corner.y, 1.0));
    o.color = color_at(t);
    o.uv = float2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
    return o;
}
#endif

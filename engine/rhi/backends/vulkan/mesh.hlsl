// Lit meshes for the Vulkan backend, first step: base colour lit by the sun (Lambert) and the sky /
// ground ambient, as the D3D12 shader's diffuse terms; no textures, shadows or specular yet.
// Everything arrives as push constants (192 bytes; every desktop GPU offers 256).
// shortcut: a subset of the D3D12 lit shader; share its HLSL (and the Lights buffer) as the port continues
struct Push {
    float4x4 mvp;
    float4 n0, n1, n2;   // normal matrix columns; n0.w = 1: unlit (colour only)
    float4 color;        // base colour, linear
    float4 sun_dir;      // where the sun's light travels (xyz), exposure (w)
    float4 sun_color;    // its colour times intensity (xyz), w = 1 when there is a sun
    float4 sky, ground;  // ambient from above / below
};
[[vk::push_constant]] Push pc;

struct VsIn {
    [[vk::location(0)]] float3 pos : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
};
struct VsOut { float4 pos : SV_Position; float3 normal : NORMAL; };

VsOut vs_main(VsIn i) {
    VsOut o;
    o.pos = mul(pc.mvp, float4(i.pos, 1));
    o.normal = pc.n0.xyz * i.normal.x + pc.n1.xyz * i.normal.y + pc.n2.xyz * i.normal.z;
    return o;
}

float3 to_srgb(float3 c) {
    c = saturate(c);
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

float4 ps_main(VsOut i, bool front : SV_IsFrontFace) : SV_Target {
    const float3 base = pc.color.rgb;
    if (pc.n0.w > 0.5) return float4(to_srgb(base), pc.color.a);
    const float3 n = normalize(i.normal) * (front ? 1.0 : -1.0);
    const float3 ambient = lerp(pc.ground.rgb, pc.sky.rgb, n.z * 0.5 + 0.5);
    float3 lit = base * ambient;
    if (pc.sun_color.w > 0.5) lit += base / 3.14159265 * pc.sun_color.rgb * saturate(dot(n, -pc.sun_dir.xyz));
    return float4(to_srgb(lit * pc.sun_dir.w), pc.color.a);
}

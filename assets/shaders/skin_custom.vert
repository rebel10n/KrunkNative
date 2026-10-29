#version 450 core
// Krunker v10 "custom" skin shaders (deob-10.0.4.js:91682 build list, :195962-196150 table): one vertex stage for every
// ported shader (assets/shaders/skin_custom/<n>.frag, built by client/src/skin_fx.c). It hands the fragment stage the
// spaces the bundle's TSL builds use: uv, positionLocal, normalLocal, the world and view positions, the clip position
// (shader 8's vPos) and the part's scale (13 / 16: length of the model matrix's columns). The normal comes from attribute 3
// (the model's own normals, client.h geometry_bind_normals); geometry without them reads (0, 0, 0), and the fragment
// stages derive the face normal from the positions (dFdx / dFdy) where they need one.
// uDisplace moves the vertex along normalLocal as the TSL positionNode of 9 and 10 (:92332-92337, :92402-92407):
//   1 (9, GasFresnal, uWobble): position + normal * (cnoise(-(uv * uScale) + time / 2) + cnoise(uv * uScale + time / 2)) / 100
//   2 (10, AngelSheel, uWarp):  position + normal * cnoise(uv * uScale + time) * uDisplacement
layout (location = 0) in vec3 v_pos;
layout (location = 1) in vec2 v_tex_coord;
layout (location = 3) in vec3 v_normal;

out vec2 vUv;
out vec3 vLocal;  // positionLocal
out vec3 vNormal; // normalLocal ((0, 0, 0) without a normal attribute)
out vec3 vView;   // positionView
out vec3 vWorld;  // (modelMatrix * position).xyz
out vec4 vClip;   // projectionMatrix * modelViewMatrix * position
out vec3 vScale;  // length(modelMatrix[0..2].xyz)

uniform mat4 transform;
uniform mat4 camera_world_inverse;
uniform mat4 camera_projection;

uniform int uDisplace; // 0 none, 1 shader 9's wobble, 2 shader 10's warp (client/src/skin_fx.c)
uniform float time;
uniform float uScale;
uniform float uDisplacement;

// cnoise (iííìíïî, deob-10.0.4.js:91622-91660): 2D classic Perlin noise, as 9.frag
vec4 kn_mod289(vec4 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4 kn_permute(vec4 x) { return kn_mod289(((x * 34.0) + 10.0) * x); }
vec4 kn_taylor_inv_sqrt(vec4 r) { return 1.79284291400159 - 0.85373472095314 * r; }
vec2 kn_fade(vec2 t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

float kn_cnoise(vec2 P) {
    vec4 Pi = floor(P.xyxy) + vec4(0.0, 0.0, 1.0, 1.0);
    vec4 Pf = fract(P.xyxy) - vec4(0.0, 0.0, 1.0, 1.0);
    Pi = kn_mod289(Pi);
    vec4 ix = Pi.xzxz;
    vec4 iy = Pi.yyww;
    vec4 fx = Pf.xzxz;
    vec4 fy = Pf.yyww;
    vec4 i = kn_permute(kn_permute(ix) + iy);
    vec4 gx = fract(i * (1.0 / 41.0)) * 2.0 - 1.0;
    vec4 gy = abs(gx) - 0.5;
    vec4 tx = floor(gx + 0.5);
    gx = gx - tx;
    vec2 g00 = vec2(gx.x, gy.x);
    vec2 g10 = vec2(gx.y, gy.y);
    vec2 g01 = vec2(gx.z, gy.z);
    vec2 g11 = vec2(gx.w, gy.w);
    vec4 norm = kn_taylor_inv_sqrt(vec4(dot(g00, g00), dot(g01, g01), dot(g10, g10), dot(g11, g11)));
    float n00 = norm.x * dot(g00, vec2(fx.x, fy.x));
    float n10 = norm.y * dot(g10, vec2(fx.y, fy.y));
    float n01 = norm.z * dot(g01, vec2(fx.z, fy.z));
    float n11 = norm.w * dot(g11, vec2(fx.w, fy.w));
    vec2 fade_xy = kn_fade(Pf.xy);
    vec2 n_x = mix(vec2(n00, n01), vec2(n10, n11), fade_xy.x);
    float n_xy = mix(n_x.x, n_x.y, fade_xy.y);
    return 2.3 * n_xy;
}

void main() {
    vUv = v_tex_coord;
    vLocal = v_pos;
    vNormal = v_normal;
    vec3 position = v_pos;
    if (uDisplace == 1) {
        float displacement = kn_cnoise(-(v_tex_coord * uScale) + time / 2.0) + kn_cnoise(v_tex_coord * uScale + time / 2.0);
        position += v_normal * displacement / 100.0;
    } else if (uDisplace == 2) {
        position += v_normal * kn_cnoise(v_tex_coord * uScale + time) * uDisplacement;
    }
    vec4 world = transform * vec4(position, 1.0);
    vWorld = world.xyz;
    vec4 view = camera_world_inverse * world;
    vView = view.xyz;
    vScale = vec3(length(transform[0].xyz), length(transform[1].xyz), length(transform[2].xyz));
    gl_Position = camera_projection * view;
    vClip = gl_Position;
}

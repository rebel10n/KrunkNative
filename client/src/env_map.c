// Krunker v10.0.3 getEnvMap and its PMREM for GLB environment reflections (client/include/env_map.h).
#include <gl.h>
#include <client.h>
#include <env_map.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define ENV_ROWS 128   // the canvas height (deob-10.0.3.js:231135): one table row per canvas row
#define ENV_LEVELS 9   // PMREMGenerator._lodMeshes for a 64 px cube: lodMax 6 - LOD_MIN 4 + 1 + 6 (deob:70799)
#define ENV_GGX_SAMPLES 256 // the GGX filter's GGX_SAMPLES (deob:70877)

static unsigned int g_env_texture;   // ENV_LEVELS x ENV_ROWS RGBA16F, linear radiance (unit BASIC_SCENE_ENV_TEXTURE_UNIT)
static unsigned int g_dfg_texture;   // 16 x 16 RG16F DFG_LUT (unit BASIC_SCENE_ENV_DFG_TEXTURE_UNIT)
static char g_env_key[32];           // getEnvMap's cache key: top + mid + fog hex (deob:231126)
static char g_dome_colors[3][8];     // skyDomeCols as "#rrggbb"
static int g_dome_count;

// three r18x's DFG_LUT (deob-10.0.3.js:69405): 16 x 16 RG half floats, u = roughness, v = dotNV
static const uint16_t env_dfg_lut[512] = {
    12469, 15057, 12620, 14925, 13266, 14620, 13807, 14376, 14323, 13990, 14545, 13625, 14713, 13328, 14840, 12882,
    14931, 12528, 14996, 12233, 15039, 11829, 15066, 11525, 15080, 11295, 15085, 10976, 15082, 10705, 15073, 10495,
    13880, 14564, 13898, 14542, 13977, 14430, 14158, 14124, 14393, 13732, 14556, 13410, 14702, 12996, 14814, 12596,
    14891, 12291, 14937, 11834, 14957, 11489, 14958, 11194, 14943, 10803, 14921, 10506, 14893, 10278, 14858, 9960,
    14484, 14039, 14487, 14025, 14499, 13941, 14524, 13740, 14574, 13468, 14654, 13106, 14743, 12678, 14818, 12344,
    14867, 11893, 14889, 11509, 14893, 11180, 14881, 10751, 14852, 10428, 14812, 10128, 14765, 9754, 14712, 9466,
    14764, 13480, 14764, 13475, 14766, 13440, 14766, 13347, 14769, 13070, 14786, 12713, 14816, 12387, 14844, 11957,
    14860, 11549, 14868, 11215, 14855, 10751, 14825, 10403, 14782, 10044, 14729, 9651, 14666, 9352, 14599, 9029,
    14967, 12835, 14966, 12831, 14963, 12804, 14954, 12723, 14936, 12564, 14917, 12347, 14900, 11958, 14886, 11569,
    14878, 11247, 14859, 10765, 14828, 10401, 14784, 10011, 14727, 9600, 14660, 9289, 14586, 8893, 14508, 8533,
    15111, 12234, 15110, 12234, 15104, 12216, 15092, 12156, 15067, 12010, 15028, 11776, 14981, 11500, 14942, 11205,
    14902, 10752, 14861, 10393, 14812, 9991, 14752, 9570, 14682, 9252, 14603, 8808, 14519, 8445, 14431, 8145,
    15209, 11449, 15208, 11451, 15202, 11451, 15190, 11438, 15163, 11384, 15117, 11274, 15055, 10979, 14994, 10648,
    14932, 10343, 14871, 9936, 14803, 9532, 14729, 9218, 14645, 8742, 14556, 8381, 14461, 8020, 14365, 7603,
    15273, 10603, 15272, 10607, 15267, 10619, 15256, 10631, 15231, 10614, 15182, 10535, 15118, 10389, 15042, 10167,
    14963, 9787, 14883, 9447, 14800, 9115, 14710, 8665, 14615, 8318, 14514, 7911, 14411, 7507, 14279, 7198,
    15314, 9675, 15313, 9683, 15309, 9712, 15298, 9759, 15277, 9797, 15229, 9773, 15166, 9668, 15084, 9487,
    14995, 9274, 14898, 8910, 14800, 8539, 14697, 8234, 14590, 7790, 14479, 7409, 14367, 7067, 14178, 6621,
    15337, 8619, 15337, 8631, 15333, 8677, 15325, 8769, 15305, 8871, 15264, 8940, 15202, 8909, 15119, 8775,
    15022, 8565, 14916, 8328, 14804, 8009, 14688, 7614, 14569, 7287, 14448, 6888, 14321, 6483, 14088, 6171,
    15350, 7402, 15350, 7419, 15347, 7480, 15340, 7613, 15322, 7804, 15287, 7973, 15229, 8057, 15148, 8012,
    15046, 7846, 14933, 7611, 14810, 7357, 14682, 7069, 14552, 6656, 14421, 6316, 14251, 5948, 14007, 5528,
    15356, 5942, 15356, 5977, 15353, 6119, 15348, 6294, 15332, 6551, 15302, 6824, 15249, 7044, 15171, 7122,
    15070, 7050, 14949, 6861, 14818, 6611, 14679, 6349, 14538, 6067, 14398, 5651, 14189, 5311, 13935, 4958,
    15359, 4123, 15359, 4153, 15356, 4296, 15353, 4646, 15338, 5160, 15311, 5508, 15263, 5829, 15188, 6042,
    15088, 6094, 14966, 6001, 14826, 5796, 14678, 5543, 14527, 5287, 14377, 4985, 14133, 4586, 13869, 4257,
    15360, 1563, 15360, 1642, 15358, 2076, 15354, 2636, 15341, 3350, 15317, 4019, 15273, 4429, 15203, 4732,
    15105, 4911, 14981, 4932, 14836, 4818, 14679, 4621, 14517, 4386, 14359, 4156, 14083, 3795, 13808, 3437,
    15360, 122, 15360, 137, 15358, 285, 15355, 636, 15344, 1274, 15322, 2177, 15281, 2765, 15215, 3223,
    15120, 3451, 14995, 3569, 14846, 3567, 14681, 3466, 14511, 3305, 14344, 3121, 14037, 2800, 13753, 2467,
    15360, 0, 15360, 1, 15359, 21, 15355, 89, 15346, 253, 15325, 479, 15287, 796, 15225, 1148,
    15133, 1492, 15008, 1749, 14856, 1882, 14685, 1886, 14506, 1783, 14324, 1608, 13996, 1398, 13702, 1183,
};

typedef struct { float r, g, b; } EnvRgb; // THREE.Color channels 0..1 (ColorManagement is off: raw sRGB values)

// new THREE.Color("#hex") with ColorManagement off (no sRGB conversion, deob:34566); an unparsable colour stays white
static EnvRgb env_color(const char *hex) {
    vec4 c = {1.0f, 1.0f, 1.0f, 1.0f};
    if (hex && hex[0]) parse_hex_color(hex, &c);
    return (EnvRgb) {c.x, c.y, c.z};
}

static EnvRgb env_lerp(const EnvRgb a, const EnvRgb b, const float t) { // Color.lerp
    return (EnvRgb) {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

static int env_byte(const float v) { // Color.getHex: round(clamp(c * 255, 0, 255))
    const float c = roundf(v * 255.0f);
    return c < 0.0f ? 0 : c > 255.0f ? 255 : (int) c;
}

static void env_hex(const EnvRgb c, char out[8]) {
    snprintf(out, 8, "#%02x%02x%02x", env_byte(c.r), env_byte(c.g), env_byte(c.b));
}

// getEnvMap's tint (deob:231137-231141): Color(c).lerp(#8c9098, 0.55).lerp(#ffffff, w), as 8-bit sRGB
static EnvRgb env_tint(const char *hex, const float w) {
    EnvRgb c = env_lerp(env_color(hex), env_color("#8c9098"), 0.55f);
    c = env_lerp(c, (EnvRgb) {1.0f, 1.0f, 1.0f}, w);
    char rounded[8];
    env_hex(c, rounded);
    return env_color(rounded);
}

static float env_srgb_decode(const float c) { // the SRGBColorSpace texture's hardware decode (SRGB8_ALPHA8, deob:93001)
    return c <= 0.04045f ? c * 0.0773993808f : powf(c * 0.9478672986f + 0.0521327014f, 2.4f);
}

// The 128 canvas rows of the 0..1 vertical gradient, decoded to linear, bottom (nadir) row first: the
// equirect v = asin(y) / PI + 0.5 runs up the image (CanvasTexture flipY), so table row j is canvas row 127 - j.
static void env_paint(const char *top, const char *mid, const char *fog, EnvRgb rows[ENV_ROWS]) {
    const float offsets[5] = {0.0f, 0.42f, 0.5f, 0.56f, 1.0f};
    EnvRgb stops[5];
    stops[0] = env_tint(top, 0.35f);
    stops[1] = env_tint(mid, 0.25f);
    stops[2] = env_tint(fog, 0.55f);
    stops[3] = env_tint(fog, 0.1f);
    const EnvRgb fog_color = env_color(fog);
    char dark[8];
    env_hex((EnvRgb) {fog_color.r * 0.25f, fog_color.g * 0.25f, fog_color.b * 0.25f}, dark); // multiplyScalar(0.25)
    stops[4] = env_color(dark);

    for (int y = 0; y < ENV_ROWS; y++) {
        const float t = ((float) y + 0.5f) / (float) ENV_ROWS; // createLinearGradient(0, 0, 0, 128) at the pixel centre
        int s = 0;
        while (s < 3 && t > offsets[s + 1]) s++;
        const float f = (t - offsets[s]) / (offsets[s + 1] - offsets[s]);
        const EnvRgb c = env_lerp(stops[s], stops[s + 1], f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f);
        // the canvas stores 8-bit sRGB (gradient dithering not modelled), decoded when sampled
        rows[ENV_ROWS - 1 - y] = (EnvRgb) {
            env_srgb_decode((float) env_byte(c.r) / 255.0f),
            env_srgb_decode((float) env_byte(c.g) / 255.0f),
            env_srgb_decode((float) env_byte(c.b) / 255.0f),
        };
    }
}

// A level's radiance towards elevation `y` (the direction's y): the rows linearly interpolated (bilinear filtering)
static EnvRgb env_level_at(const EnvRgb rows[ENV_ROWS], float y) {
    y = y < -1.0f ? -1.0f : y > 1.0f ? 1.0f : y;
    float row = (asinf(y) / (float) M_PI + 0.5f) * (float) ENV_ROWS - 0.5f;
    if (row < 0.0f) row = 0.0f;
    if (row > (float) (ENV_ROWS - 1)) row = (float) (ENV_ROWS - 1);
    const int i = (int) row;
    const int j = i + 1 < ENV_ROWS ? i + 1 : i;
    return env_lerp(rows[i], rows[j], row - (float) i);
}

static float env_radical_inverse(uint32_t bits) { // the Hammersley sequence's van der Corput radical inverse (deob:70532-70547)
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float) bits * 2.3283064365386963e-10f;
}

// PMREMGenerator._applyGGXFilter (deob:70984-71011) with the ggx filter node (deob:70565-70601) for one level:
// out = sum(prev(L) * NdotL) / sum(NdotL) over GGX_SAMPLES importance samples of GGX (alpha = roughness^2,
// V = N), roughness = sqrt(r_i^2 - r_{i-1}^2) * (r_i * 1.25), r_i = i / (levels - 1).
static void env_ggx_level(const EnvRgb prev[ENV_ROWS], EnvRgb out[ENV_ROWS], const float roughness) {
    if (roughness < 0.001f) {
        memcpy(out, prev, sizeof(EnvRgb) * ENV_ROWS);
        return;
    }
    const float alpha = roughness * roughness;
    for (int j = 0; j < ENV_ROWS; j++) {
        const float e = (((float) j + 0.5f) / (float) ENV_ROWS - 0.5f) * (float) M_PI; // the row's elevation
        const vec3 n = {cosf(e), sinf(e), 0.0f};
        // up = |N.z| < 0.999 ? (0, 0, 1) : (1, 0, 0); tangent = normalize(cross(up, N)); bitangent = cross(N, tangent)
        const vec3 t = {-n.y, n.x, 0.0f};
        const vec3 b = {n.y * t.z - n.z * t.y, n.z * t.x - n.x * t.z, n.x * t.y - n.y * t.x};
        EnvRgb sum = {0.0f, 0.0f, 0.0f};
        float weight = 0.0f;
        for (int s = 0; s < ENV_GGX_SAMPLES; s++) {
            const float u = (float) s / (float) ENV_GGX_SAMPLES, v = env_radical_inverse((uint32_t) s);
            // the GGX VNDF sample with V = (0, 0, 1) (deob:70548-70563)
            const float r = sqrtf(u), phi = 2.0f * 3.14159265359f * v;
            const float t1 = r * cosf(phi), t2 = r * sinf(phi);
            const float hz = sqrtf(fmaxf(0.0f, 1.0f - t1 * t1 - t2 * t2));
            float hx = alpha * t1, hy = alpha * t2;
            const float hl = sqrtf(hx * hx + hy * hy + hz * hz);
            hx /= hl;
            hy /= hl;
            const float hzn = hz / hl;
            vec3 h = {t.x * hx + b.x * hy + n.x * hzn, t.y * hx + b.y * hy + n.y * hzn, t.z * hx + b.z * hy + n.z * hzn};
            const float h_len = sqrtf(h.x * h.x + h.y * h.y + h.z * h.z);
            h = (vec3) {h.x / h_len, h.y / h_len, h.z / h_len};
            const float nh = n.x * h.x + n.y * h.y + n.z * h.z;
            vec3 l = {h.x * 2.0f * nh - n.x, h.y * 2.0f * nh - n.y, h.z * 2.0f * nh - n.z};
            const float l_len = sqrtf(l.x * l.x + l.y * l.y + l.z * l.z);
            l = (vec3) {l.x / l_len, l.y / l_len, l.z / l_len};
            const float nl = fmaxf(n.x * l.x + n.y * l.y + n.z * l.z, 0.0f);
            if (nl <= 0.0f) continue;
            const EnvRgb c = env_level_at(prev, l.y);
            sum.r += c.r * nl;
            sum.g += c.g * nl;
            sum.b += c.b * nl;
            weight += nl;
        }
        out[j] = weight > 0.0f ? (EnvRgb) {sum.r / weight, sum.g / weight, sum.b / weight} : prev[j];
    }
}

static void env_upload(const EnvRgb levels[ENV_LEVELS][ENV_ROWS]) {
    static float pixels[ENV_ROWS][ENV_LEVELS][4];
    for (int j = 0; j < ENV_ROWS; j++) {
        for (int i = 0; i < ENV_LEVELS; i++) {
            pixels[j][i][0] = levels[i][j].r;
            pixels[j][i][1] = levels[i][j].g;
            pixels[j][i][2] = levels[i][j].b;
            pixels[j][i][3] = 1.0f;
        }
    }
    // a unit of its own: unit 0's binding is tracked in g_active_texture
    glActiveTexture(GL_TEXTURE0 + BASIC_SCENE_ENV_TEXTURE_UNIT);
    if (!g_env_texture) {
        glGenTextures(1, &g_env_texture);
        glBindTexture(GL_TEXTURE_2D, g_env_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, g_env_texture);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, ENV_LEVELS, ENV_ROWS, 0, GL_RGBA, GL_FLOAT, pixels);

    if (!g_dfg_texture) {
        glActiveTexture(GL_TEXTURE0 + BASIC_SCENE_ENV_DFG_TEXTURE_UNIT);
        glGenTextures(1, &g_dfg_texture);
        glBindTexture(GL_TEXTURE_2D, g_dfg_texture);
        // DataTexture(data, 16, 16, RGFormat, HalfFloatType), Linear filters, clamped (deob:69410-69417)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, 16, 16, 0, GL_RG, GL_HALF_FLOAT, env_dfg_lut);
    }
    glActiveTexture(GL_TEXTURE0);
}

void env_map_note_sky_dome(const char *const *colors, const int count) {
    g_dome_count = 0;
    for (int i = 0; colors && i < count && i < 3; i++) {
        if (!colors[i] || !colors[i][0]) continue;
        snprintf(g_dome_colors[g_dome_count++], sizeof(g_dome_colors[0]), "%s", colors[i]);
    }
}

void env_map_update(const vec4 *fog_color) {
    // var fog = scene && scene.fog ? "#" + scene.fog.color.getHexString() : "#8899aa"
    char fog[8] = "#8899aa";
    if (fog_color) env_hex((EnvRgb) {fog_color->x, fog_color->y, fog_color->z}, fog);
    // top = cols[1] || cols[0] || fog, mid = cols[0] || fog (deob:231124-231125)
    const char *top = g_dome_count > 1 ? g_dome_colors[1] : g_dome_count ? g_dome_colors[0] : fog;
    const char *mid = g_dome_count ? g_dome_colors[0] : fog;
    char key[32];
    snprintf(key, sizeof(key), "%s%s%s", top, mid, fog);
    if (g_env_texture && !strcmp(key, g_env_key)) return;

    static EnvRgb levels[ENV_LEVELS][ENV_ROWS];
    env_paint(top, mid, fog, levels[0]);
    for (int i = 1; i < ENV_LEVELS; i++) {
        const float r = (float) i / (float) (ENV_LEVELS - 1), r_prev = (float) (i - 1) / (float) (ENV_LEVELS - 1);
        env_ggx_level(levels[i - 1], levels[i], sqrtf(r * r - r_prev * r_prev) * (r * 1.25f));
    }
    env_upload(levels);
    snprintf(g_env_key, sizeof(g_env_key), "%s", key);
    basic_material_set_scene_env(g_env_texture, g_dfg_texture);
}

void env_map_fini(void) {
    basic_material_set_scene_env(0, 0);
    if (g_env_texture) glDeleteTextures(1, &g_env_texture);
    if (g_dfg_texture) glDeleteTextures(1, &g_dfg_texture);
    g_env_texture = g_dfg_texture = 0;
    g_env_key[0] = 0;
}

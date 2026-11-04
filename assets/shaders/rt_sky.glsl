// Box Office physical sky (rtSky, rtMode > 0; client/src/sky_physical.c, doc/raytracing.md "Physical sky").
// Included by rt_sky.frag (the sky pass), rt_reflect.frag / rt_gi.frag (their ray misses) and basic.frag (the
// aerial-perspective tint of krunker_fog). sky_physical.c mirrors psky_scatter / psky_authored /
// psky_base in C for the PMREM table (env_map.c) and the day factor, so keep both in step.
//
// The model keeps the map's authored mood: psky_authored is Krunker's own gradient (the sky dome's three stops,
// or the clear colour), and the analytic single-scattering atmosphere (Rayleigh + Mie, air-mass optical depth)
// only shapes it: on a day map (psky_day 1) the colour comes mostly from the scattering at the authored
// brightness; on a night map (psky_day 0) the authored hue stays and the scattering only modulates brightness
// (a moon glow), with faint stars. Clouds are a drifting fbm layer, the sun (or moon) a limb-darkened disk in the
// shadow direction. psky_on 0 (the uniform's default) leaves every caller on its old path.
uniform int psky_on;
uniform vec3 psky_sun;           // normalised, towards the sun (g_render_lighting.light_direction)
uniform vec3 psky_sun_color;     // the sun after the atmosphere, luminance 1 (reddens at low elevation)
uniform vec3 psky_stop_top;      // the authored gradient: zenith, horizon, nadir
uniform vec3 psky_stop_mid;
uniform vec3 psky_stop_bottom;
uniform float psky_avg;          // mean luminance of psky_authored over the upper hemisphere
uniform float psky_day;          // 0 night .. 1 day
uniform float psky_disk_cos;     // cos(the disk's angular radius)
uniform float psky_time;         // seconds (cloud drift; advances with textureAnim like Krunker's sky scroll)
uniform float psky_clouds;       // cloud opacity (0: none)
uniform int psky_cine;            // Cinematic preset (rt.h "Cinematic colour pipeline"): saturated zenith, fuller clouds, stronger aureole

float psky_lum(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// Krunker's SkyDome getGradient (sky.frag): v = the sphere's uv.y = 0.5 + asin(y) / PI
vec3 psky_authored(vec3 d) {
    float v = 0.5 + asin(clamp(d.y, -1.0, 1.0)) * 0.3183098862;
    return v < 0.5 ? mix(psky_stop_bottom, psky_stop_mid, v / 0.5) : mix(psky_stop_mid, psky_stop_top, (v - 0.5) / 0.5);
}

float psky_airmass(float elevation) {
    return 1.0 / (max(elevation, 0.0) + 0.06);
}

// Analytic single scattering: Rayleigh (1 / lambda^4, normalised to blue) + grey Mie (Henyey-Greenstein g 0.76),
// the sun's light attenuated over its own air mass, in-scattered over the view's.
vec3 psky_scatter(vec3 d) {
    const vec3 rayleigh = vec3(0.17, 0.40, 1.0) * 0.35;
    const float mie = 0.008;
    vec3 beta = rayleigh + vec3(mie);
    float mu = max(d.y, 0.0);
    vec3 sun_t = exp(-beta * psky_airmass(psky_sun.y));
    float c = dot(d, psky_sun);
    float pr = 0.0596831 * (1.0 + c * c);
    const float g = 0.76;
    float pm = 0.0795775 * (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * c, 1.5);
    vec3 inscatter = sun_t * (rayleigh * pr + vec3(mie * pm)) / beta * (vec3(1.0) - exp(-beta * psky_airmass(mu)));
    if (d.y < 0.0) inscatter *= 1.0 - 0.7 * smoothstep(0.0, 0.4, -d.y); // below the horizon: towards the ground
    return inscatter;
}

vec3 psky_chroma(vec3 c) {
    float l = psky_lum(c);
    return l > 1e-4 ? c / l : vec3(1.0);
}

// The sun's aureole as a brightness factor: a sharp Henyey-Greenstein lobe (g 0.95, scaled by 4 PI) saturated
// smoothly (x / (1 + x)), so it falls off over a few degrees instead of clipping a wide plateau.
float psky_glow(vec3 d, float k) {
    const float g = 0.95;
    float c = dot(d, psky_sun);
    float hg = 0.0795775 * (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * c, 1e-6), 1.5) * 12.566371;
    float x = k * (psky_cine != 0 ? 2.5 : 1.0) * max(hg - 1.0, 0.0);
    return 1.0 + (psky_cine != 0 ? 0.6 : 0.35) * x / (1.0 + x);
}

// The sky without clouds, stars and the disk (what sky_physical.c mirrors for the PMREM table). Brightness comes
// from the authored gradient (so the lighting keeps Krunker's exposure); a day map takes the scattering's hue
// (blue zenith, pale horizon, reddening at a low sun), 40 % of the way back to the authored hue;
// both get the sun's glow.
// sky_physical.c psky_keep: the authored hue's share of a day sky, higher where the authored colour is saturated
float psky_keep(vec3 a) {
    float mx = max(a.r, max(a.g, a.b)), mn = min(a.r, min(a.g, a.b));
    float sat = mx > 1e-4 ? (mx - mn) / mx : 0.0;
    return mix(psky_cine != 0 ? 0.15 : 0.4, 0.85, smoothstep(0.08, 0.25, sat));
}

vec3 psky_base(vec3 d) {
    vec3 a = psky_authored(d);
    // single scattering leaves the horizon cyan; multiple scattering whitens it: blend the hue towards white there
    vec3 hue = mix(psky_chroma(psky_scatter(d)), vec3(1.0), 0.75 * exp(-max(d.y, 0.0) * 6.0));
    vec3 day = mix(hue * psky_lum(a), a, psky_keep(a));
    if (psky_cine != 0) day = max(mix(vec3(psky_lum(day)), day, 1.2), vec3(0.0)); // sky_physical.c mirrors this
    return mix(a, day, psky_day) * psky_glow(d, mix(0.001, 0.004, psky_day));
}

float psky_hash(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float psky_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = psky_hash(vec3(i, 0.0));
    float b = psky_hash(vec3(i + vec2(1.0, 0.0), 0.0));
    float c = psky_hash(vec3(i + vec2(0.0, 1.0), 0.0));
    float e = psky_hash(vec3(i + vec2(1.0, 1.0), 0.0));
    return mix(mix(a, b, u.x), mix(c, e, u.x), u.y);
}

float psky_fbm(vec2 p) {
    float sum = 0.0, amp = 0.5;
    for (int i = 0; i < 5; i++) {
        sum += amp * psky_noise(p);
        p = p * 2.03 + vec2(17.1, 9.7);
        amp *= 0.5;
    }
    return sum;
}

// psky_fbm for the cloud cover, which only reads it through smoothstep(lo, hi, n): every octave adds a value in
// [0, amp], so once the partial sum plus every remaining amplitude stays below lo, or the partial sum alone
// reaches hi, the smoothstep is exactly 0 or 1 whatever the rest adds and the loop stops. Same result as
// smoothstep(lo, hi, psky_fbm(p)), fewer octaves on clear sky and solid cloud.
float psky_fbm_cover(vec2 p, float lo, float hi) {
    float sum = 0.0, amp = 0.5;
    for (int i = 0; i < 5; i++) {
        sum += amp * psky_noise(p);
        p = p * 2.03 + vec2(17.1, 9.7);
        amp *= 0.5;
        float rest = amp * 2.0; // amp + amp / 2 + ... over the remaining octaves, bounded above by 2 amp
        if (sum + rest < lo || sum >= hi) break;
    }
    return smoothstep(lo, hi, sum);
}

// The full sky towards `d`: the base, faint stars at night, the cloud layer, the disk (with_disk 0: GI misses,
// where the sun is the direct term already).
vec3 psky_radiance(vec3 d, int with_disk) {
    vec3 col = psky_base(d);
    vec3 a = psky_authored(d);
    float cover = 0.0;
    if (d.y > 0.0 && psky_clouds > 0.0) {
        vec2 p = d.xz / (d.y + 0.08) * 1.6 + vec2(psky_time * 0.010, psky_time * 0.004);
        cover = (psky_cine != 0 ? psky_fbm_cover(p, 0.45, 0.74) : psky_fbm_cover(p, 0.52, 0.78)) * smoothstep(0.02, 0.25, d.y) * psky_clouds;
        float forward = pow(max(dot(d, psky_sun), 0.0), 8.0);
        vec3 lit = vec3(psky_lum(a) + 0.15) * mix(vec3(1.0), psky_sun_color, 0.5) * 1.15;
        vec3 cloud = mix(a * 0.85, lit, psky_day) * (0.85 + 0.3 * forward);
        col = mix(col, cloud, cover * (psky_cine != 0 ? 0.85 : 0.7));
    }
    if (d.y > 0.0 && psky_day < 1.0) {
        vec3 q = d * 180.0;
        vec3 f = fract(q) - 0.5;
        float star = step(0.9965, psky_hash(floor(q))) * smoothstep(0.35, 0.0, length(f));
        col += vec3(0.6) * star * (1.0 - psky_day) * smoothstep(0.05, 0.3, d.y) * (1.0 - cover);
    }
    if (with_disk != 0 && psky_sun.y > -0.05) {
        float c = dot(d, psky_sun);
        float span = 1.0 - psky_disk_cos;
        if (c > psky_disk_cos - span * 0.2) {
            float r2 = clamp((1.0 - c) / span, 0.0, 1.0);
            float limb = 1.0 - 0.6 * (1.0 - sqrt(1.0 - r2));
            float edge = smoothstep(psky_disk_cos - span * 0.2, psky_disk_cos + span * 0.2, c);
            vec3 disk = mix(vec3(1.0), psky_sun_color, psky_day) * limb * mix(1.6, 10.0, psky_day);
            col = mix(col, disk, edge * (1.0 - cover * 0.8));
        }
    }
    return col;
}

// Aerial perspective for krunker_fog: the fog brightens and warms towards the sun (Mie forward scattering) and
// dims slightly away from it; normalised so the mean over all directions stays about 1.
vec3 psky_fog_color(vec3 d, vec3 fog) {
    float c = dot(d, psky_sun);
    const float g = 0.6;
    float hg = 0.0795775 * (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * c, 1.5) * 12.566371;
    float k = mix(0.12, 0.45, psky_day);
    vec3 tint = mix(vec3(1.0), psky_sun_color, clamp((hg - 1.0) * 0.25, 0.0, 1.0) * psky_day);
    return fog * tint * clamp(mix(1.0, hg, k), 0.7, 2.2);
}

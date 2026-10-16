#version 450 core
// HTML/CSS engine fragment shader (client/src/html/engine/gl_backend.c).
// One program for every display-list primitive; `v_mode` selects the path.
// Output is premultiplied alpha, blended with GL_ONE / GL_ONE_MINUS_SRC_ALPHA.
//
// Modes (see gl_backend.c for the per-vertex payload of each):
//   0 solid            color only
//   1 rounded rect     uv = local px from rect center; p0 = radii tl,tr,br,bl; p1.xy = half size
//   2 image            uv = texcoord; p0.x = 1 when the texture is straight alpha; p0.y = 1: AA edges at target rect p2
//   3 glyph            uv = atlas texcoord (R8) x gamma LUT row by text luminance
//   4 gradient         uv = local px; p0 = (kind, dir.x, dir.y, 1/len) | (1, 1/rx, 1/ry, 0); p1 = (cx, cy, lut v, aa);
//                      p1.w = 1: AA edges at target rect p2
//   5 box-shadow       uv = local px from shadow rect center; p0 = shadow radii; p1 = (hw, hh, sigma, box radius delta);
//                      p2 = (box cx, box cy, box hw, box hh): the element box is cut out
//   6 border           uv = local px from border-box center; p0 = outer radii; p1 = (hw, hh, style + 4*edge, period); p2 = widths t,r,b,l
//   7 group composite  uv = texcoord of the group FBO; u_opacity x u_cm color matrix; p0.x = 1 -> shadow (color x alpha)
//   8 blur pass        uv = texcoord; separable 7-tap Gaussian along u_blur_dir (internal), taps clamped
//                      to u_blur_clamp (edge mode clamp); u_blur_dir = 0 is a plain bilinear resample
//   9 inset box-shadow like 5 with inverted shadow coverage, clipped to the box in p2
//  10 masked group     mode 7 (color matrix path) x the alpha of u_mask (unit 2) at the same content pixel:
//                      mask uv = (u * p1.x, 1 - (1 - v) * p1.y) (the two pool entries' size ratios)
//
// Clip: every fragment is also multiplied by the coverage of the per-vertex
// rounded clip rect (v_clip, v_clip_radius) in target pixels, and by that of
// the clip-path polygon in u_poly_edges (u_poly_n > 0), anti-aliased like Skia.
in vec2 v_pos;
in vec2 v_uv;
in vec4 v_color;
flat in vec4 v_p0;
flat in vec4 v_p1;
flat in vec4 v_p2;
flat in vec4 v_clip;
flat in float v_clip_radius;
flat in float v_mode;

uniform sampler2D u_tex;        // unit 0: image / glyph atlas / gradient LUT / group texture
uniform sampler2D u_gamma;      // unit 1: 256x8 R8 text gamma table
uniform mat4 u_cm;              // group color matrix (rgba x rgba)
uniform vec4 u_cm_offset;       // group color matrix translation column
uniform float u_opacity;        // group opacity
uniform vec2 u_blur_dir;        // blur pass: step between taps in texcoords
uniform vec4 u_blur_clamp;      // blur pass: texcoord clamp rect (u0, v0, u1, v1)
uniform sampler2D u_mask;       // unit 2: mask of a masked group (mode 10)
uniform sampler2D u_backdrop;   // unit 3: the target under a blend-mode group (mode 11)
uniform int u_blend;            // mode 11: CssBlendMode (1 multiply .. 15 luminosity)
uniform int u_stencil_pass;     // 1: discard fragments under half coverage (stencil shape pass)
// clip-path: polygon(): its boundary as edges (x0, y0, x1, y1) in target pixels, the two
// traversals of a ring bridge (Krunker Strike's frames) cancelled out (gl_backend.c)
#define POLY_MAX_EDGES 64
uniform int u_poly_n;           // edges; 0 = no polygon clip
uniform int u_poly_evenodd;     // fill rule: 0 nonzero, 1 evenodd
uniform vec4 u_poly_edges[POLY_MAX_EDGES];

out vec4 frag_color;

// Signed distance to a rounded rect centred at the origin; r = tl,tr,br,bl (y down).
float sd_rrect(vec2 p, vec2 h, vec4 r) {
    float rr = p.x < 0.0 ? (p.y < 0.0 ? r.x : r.w) : (p.y < 0.0 ? r.y : r.z);
    rr = min(rr, min(h.x, h.y));
    vec2 q = abs(p) - h + rr;
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - rr;
}

// Anti-aliased coverage of distance d, where one pixel spans `px` local units.
float aa(float d, float px) {
    return clamp(0.5 - d / max(px, 1e-4), 0.0, 1.0);
}

// Coverage of the target-pixel rect r = (x0, y0, x1, y1) at this fragment
// (box-filtered, so fractional edges are anti-aliased like Chrome's).
float rect_coverage(vec4 r) {
    float cx = clamp(min(v_pos.x - r.x, r.z - v_pos.x) + 0.5, 0.0, 1.0);
    float cy = clamp(min(v_pos.y - r.y, r.w - v_pos.y) + 0.5, 0.0, 1.0);
    return cx * cy;
}

// Local units per pixel of the uv (local space) parametrisation.
float local_px() {
    vec2 dx = dFdx(v_uv);
    vec2 dy = dFdy(v_uv);
    return max(length(vec2(dx.x, dy.x)), 1e-4);
}

// Local units per pixel across the edge of distance field d: local_px() under
// uniform scale / rotation; under an anisotropic transform (a rotateY flip
// flattens x only, scaleX) the screen-space gradient of d, so edges along the
// squeezed axis keep a one-pixel AA band instead of the larger axis' one.
float edge_px(float d, float px) {
    vec2 dx = dFdx(v_uv);
    vec2 dy = dFdy(v_uv);
    float lx = length(vec2(dx.x, dy.x));
    float ly = length(vec2(dx.y, dy.y));
    float g = length(vec2(dFdx(d), dFdy(d)));
    return abs(lx - ly) > 0.02 * max(lx, ly) ? max(g, 1e-4) : px;
}

// Evan Wallace, "Fast Rounded Rectangle Shadows" (2016).
vec4 erf4(vec4 x) {
    vec4 s = sign(x), a = abs(x);
    x = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    x *= x;
    return s - s / (x * x);
}

float gaussian(float x, float sigma) {
    const float pi = 3.141592653589793;
    return exp(-(x * x) / (2.0 * sigma * sigma)) / (sqrt(2.0 * pi) * sigma);
}

float shadow_x(float x, float y, float sigma, float corner, vec2 half_size) {
    float delta = min(half_size.y - corner - abs(y), 0.0);
    float curved = half_size.x - corner + sqrt(max(0.0, corner * corner - delta * delta));
    vec2 integral = 0.5 + 0.5 * erf4(vec4(x + vec2(-curved, curved), 0.0, 0.0) * (sqrt(0.5) / sigma)).xy;
    return integral.y - integral.x;
}

// Blurred coverage of a rounded rect (corner radius picked per quadrant).
float rrect_shadow(vec2 p, vec2 half_size, vec4 radii, float sigma) {
    float corner = p.x < 0.0 ? (p.y < 0.0 ? radii.x : radii.w) : (p.y < 0.0 ? radii.y : radii.z);
    corner = min(corner, min(half_size.x, half_size.y));
    float low = p.y - half_size.y;
    float high = p.y + half_size.y;
    float start = clamp(-3.0 * sigma, low, high);
    float end = clamp(3.0 * sigma, low, high);
    float step = (end - start) / 4.0;
    float y = start + step * 0.5;
    float value = 0.0;
    for (int i = 0; i < 4; i++) {
        value += shadow_x(p.x, p.y - y, sigma, corner, half_size) * gaussian(y, sigma) * step;
        y += step;
    }
    return value;
}

float shadow_coverage(vec2 p, vec2 half_size, vec4 radii, float sigma, float px) {
    if (sigma < 0.05) return aa(sd_rrect(p, half_size, radii), px);
    return rrect_shadow(p, half_size, radii, sigma);
}

// Coverage of the clip polygon at p: inside by the winding of its edges (nonzero or
// evenodd), anti-aliased over one pixel by the distance to the nearest edge.
float poly_coverage(vec2 p) {
    int wind = 0;
    float dmin = 1e9;   // distance to the nearest edge, in pixel-box units of its direction
    for (int i = 0; i < POLY_MAX_EDGES; i++) {
        if (i >= u_poly_n) break;
        vec4 e = u_poly_edges[i];
        vec2 a = e.xy, d = e.zw - e.xy, w = p - a;
        float t = clamp(dot(w, d) / max(dot(d, d), 1e-12), 0.0, 1.0);
        // a box-filtered (area) coverage ramp across an edge spans |nx| + |ny| pixels (1 when
        // axis-aligned, sqrt 2 at 45 degrees), as Skia's analytic coverage does
        vec2 n = abs(d) / max(length(d), 1e-6);
        dmin = min(dmin, length(w - d * t) / (n.x + n.y));
        float side = d.x * w.y - d.y * w.x;
        if (a.y <= p.y) {
            if (e.w > p.y && side > 0.0) wind++;
        } else if (e.w <= p.y && side < 0.0) {
            wind--;
        }
    }
    bool inside = u_poly_evenodd != 0 ? (abs(wind) % 2) == 1 : wind != 0;
    return clamp(0.5 + (inside ? dmin : -dmin), 0.0, 1.0);
}

float clip_coverage() {
    vec2 c = 0.5 * (v_clip.xy + v_clip.zw);
    vec2 h = 0.5 * (v_clip.zw - v_clip.xy);
    return aa(sd_rrect(v_pos - c, h, vec4(v_clip_radius)), 1.0);
}

// Dash / dot pattern along an edge. s: coordinate along the edge from its
// start, t: distance from the edge's center line (across), w: edge width.
float edge_pattern(float style, float s, float t, float w, float period, float px) {
    if (style < 1.5) return 1.0;                       // solid
    if (style < 2.5) {                                  // dashed: dash = 3/5 period
        float f = mod(s, period);
        float dash = period * 0.6;
        float d = max(-f, f - dash);                    // negative inside the dash
        return aa(d, px);
    }
    float k = floor(s / period);                        // dotted: discs of diameter w
    float sc = (k + 0.5) * period;
    float d = length(vec2(s - sc, t)) - 0.5 * w;
    return aa(d, px);
}

// Image sampling (mode 2), matching Chrome's default image filter quality
// (Skia "medium": SkMipmapMode::kNearest + bilinear). SkMipmap::ComputeLevel
// takes the level from the inverse scale of the most minified axis, FLOORED:
// level = floor(log2(texels per pixel)), so the larger (sharper) level is used
// until a full halving; that one level is then filtered bilinearly. GL's own
// LOD (trilinear, or LINEAR_MIPMAP_NEAREST which rounds lambda + 0.5) switches
// to the smaller level earlier and loses the sprites' dark outlines.
// uidiff, full suite: this 306/476 passing, trilinear 272, trilinear with a
// -0.5 bias 306 but a higher total diff.
// Smooth images carry a full mip chain (image.c); the pixelated twin has
// MAX_LEVEL 0 and GL_NEAREST, so any lod reads its level 0. Every texture
// drawn in mode 2 must be mipmap complete (only image.c produces them).
vec4 sample_image(vec2 uv) {
    vec2 size = vec2(textureSize(u_tex, 0));
    vec2 dx = dFdx(uv) * size;
    vec2 dy = dFdy(uv) * size;
    float rho = max(length(vec2(dx.x, dy.x)), length(vec2(dx.y, dy.y)));
    float lod = floor(log2(max(rho, 1e-6)) + 1e-4);    // epsilon: exact halvings pick the halved level
    return textureLod(u_tex, uv, max(lod, 0.0));
}

vec4 blur_tap(vec2 uv) {
    return texture(u_tex, clamp(uv, u_blur_clamp.xy, u_blur_clamp.zw));
}

// Compositing and Blending 1, section 10: B(Cb, Cs) for the separable and non-separable modes.
float blend_soft(float cb, float cs) {
    if (cs <= 0.5) return cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb);
    float d = cb <= 0.25 ? ((16.0 * cb - 12.0) * cb + 4.0) * cb : sqrt(cb);
    return cb + (2.0 * cs - 1.0) * (d - cb);
}
float blend_dodge(float cb, float cs) { return cb <= 0.0 ? 0.0 : (cs >= 1.0 ? 1.0 : min(1.0, cb / (1.0 - cs))); }
float blend_burn(float cb, float cs) { return cb >= 1.0 ? 1.0 : (cs <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - cb) / cs)); }
vec3 blend_hard(vec3 cb, vec3 cs) {
    vec3 m = cb * 2.0 * cs, s = cb + (2.0 * cs - 1.0) - cb * (2.0 * cs - 1.0);
    return mix(m, s, vec3(greaterThan(cs, vec3(0.5))));
}
float lum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }
vec3 clip_color(vec3 c) {
    float l = lum(c), n = min(min(c.r, c.g), c.b), x = max(max(c.r, c.g), c.b);
    if (n < 0.0) c = l + (c - l) * l / max(l - n, 1e-6);
    if (x > 1.0) c = l + (c - l) * (1.0 - l) / max(x - l, 1e-6);
    return c;
}
vec3 set_lum(vec3 c, float l) { return clip_color(c + (l - lum(c))); }
float sat(vec3 c) { return max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b); }
vec3 set_sat(vec3 c, float s) {
    float mx = max(max(c.r, c.g), c.b), mn = min(min(c.r, c.g), c.b);
    return mx > mn ? (c - mn) * s / (mx - mn) : vec3(0.0);
}
vec3 blend_mode(int m, vec3 cb, vec3 cs) {
    if (m == 1) return cb * cs;
    if (m == 2) return cb + cs - cb * cs;
    if (m == 3) return blend_hard(cs, cb);             // overlay = hard-light with the layers swapped
    if (m == 4) return min(cb, cs);
    if (m == 5) return max(cb, cs);
    if (m == 6) return vec3(blend_dodge(cb.r, cs.r), blend_dodge(cb.g, cs.g), blend_dodge(cb.b, cs.b));
    if (m == 7) return vec3(blend_burn(cb.r, cs.r), blend_burn(cb.g, cs.g), blend_burn(cb.b, cs.b));
    if (m == 8) return blend_hard(cb, cs);
    if (m == 9) return vec3(blend_soft(cb.r, cs.r), blend_soft(cb.g, cs.g), blend_soft(cb.b, cs.b));
    if (m == 10) return abs(cb - cs);
    if (m == 11) return cb + cs - 2.0 * cb * cs;
    if (m == 12) return set_lum(set_sat(cs, sat(cb)), lum(cb));
    if (m == 13) return set_lum(set_sat(cb, sat(cs)), lum(cb));
    if (m == 14) return set_lum(cs, lum(cb));
    if (m == 15) return set_lum(cb, lum(cs));
    return cs;
}

vec4 premul_rgba(vec4 c) {
    return vec4(c.rgb * c.a, c.a);
}

vec4 unpremul(vec4 c) {
    return c.a > 1e-5 ? vec4(c.rgb / c.a, c.a) : vec4(0.0);
}

void main() {
    int mode = int(v_mode + 0.5);
    vec4 color = v_color;
    float cov = 1.0;

    if (mode == 1) {
        float d = sd_rrect(v_uv, v_p1.xy, v_p0);
        cov = aa(d, edge_px(d, local_px()));
    } else if (mode == 2) {
        vec4 t = sample_image(v_uv);
        if (v_p0.x > 0.5) t = premul_rgba(t);
        color = t * v_color;
        if (v_p0.y > 0.5) cov = rect_coverage(v_p2);
    } else if (mode == 3) {
        float a = texture(u_tex, v_uv).r;
        vec3 rgb = v_color.a > 1e-5 ? v_color.rgb / v_color.a : vec3(0.0);
        float lum = dot(rgb, vec3(0.299, 0.587, 0.114));
        float row = (floor(clamp(lum, 0.0, 1.0) * 7.0 + 0.5) + 0.5) / 8.0;
        cov = texture(u_gamma, vec2((a * 255.0 + 0.5) / 256.0, row)).r;
    } else if (mode == 4) {
        float t;
        if (v_p0.x < 0.5) t = 0.5 + dot(v_uv - v_p1.xy, v_p0.yz) * v_p0.w;
        else t = length((v_uv - v_p1.xy) * v_p0.yz);
        color = texture(u_tex, vec2(clamp(t, 0.0, 1.0), v_p1.z)) * v_color;
        if (v_p1.w > 0.5) cov = rect_coverage(v_p2);
    } else if (mode == 5) {
        float px = local_px();
        float s = shadow_coverage(v_uv, v_p1.xy, v_p0, v_p1.z, px);
        vec4 box_r = max(v_p0 + vec4(v_p1.w), vec4(0.0));
        float inside = aa(sd_rrect(v_uv - v_p2.xy, v_p2.zw, box_r), px);
        cov = s * (1.0 - inside);
    } else if (mode == 9) {
        float px = local_px();
        float s = shadow_coverage(v_uv, v_p1.xy, v_p0, v_p1.z, px);
        vec4 box_r = max(v_p0 + vec4(v_p1.w), vec4(0.0));
        float inside = aa(sd_rrect(v_uv - v_p2.xy, v_p2.zw, box_r), px);
        cov = (1.0 - s) * inside;
    } else if (mode == 6) {
        float px = local_px();
        vec2 h = v_p1.xy;
        vec4 w = v_p2;                                  // top, right, bottom, left
        float od = sd_rrect(v_uv, h, v_p0);
        float outer = aa(od, edge_px(od, px));
        vec2 ih = h - 0.5 * vec2(w.w + w.y, w.x + w.z);
        vec2 ic = 0.5 * vec2(w.w - w.y, w.x - w.z);
        vec4 ir = max(v_p0 - vec4(max(w.x, w.w), max(w.x, w.y), max(w.z, w.y), max(w.z, w.w)), vec4(0.0));
        float id = sd_rrect(v_uv - ic, max(ih, vec2(1e-4)), ir);
        float ipx = edge_px(id, px);                    // derivatives outside the branch below
        float inner = 0.0;
        if (ih.x > 0.0 && ih.y > 0.0) inner = aa(id, ipx);
        float style = mod(v_p1.z, 4.0);
        float edge = floor(v_p1.z / 4.0);
        float s, t, ew;
        if (edge < 0.5) { s = v_uv.x + h.x; t = v_uv.y + h.y - 0.5 * w.x; ew = w.x; }
        else if (edge < 1.5) { s = v_uv.y + h.y; t = h.x - v_uv.x - 0.5 * w.y; ew = w.y; }
        else if (edge < 2.5) { s = v_uv.x + h.x; t = h.y - v_uv.y - 0.5 * w.z; ew = w.z; }
        else { s = v_uv.y + h.y; t = v_uv.x + h.x - 0.5 * w.w; ew = w.w; }
        cov = outer * (1.0 - inner) * edge_pattern(style, s, t, ew, v_p1.w, px);
    } else if (mode == 7) {
        vec4 t = texture(u_tex, v_uv);
        if (v_p0.x > 0.5) {
            color = v_color * t.a;
        } else {
            vec4 u = unpremul(t);
            u = clamp(u_cm * u + u_cm_offset * step(1e-5, t.a), 0.0, 1.0);
            color = premul_rgba(u) * u_opacity;
        }
    } else if (mode == 8) {
        vec4 sum = blur_tap(v_uv) * 0.2707;
        sum += (blur_tap(v_uv + u_blur_dir) + blur_tap(v_uv - u_blur_dir)) * 0.2168;
        sum += (blur_tap(v_uv + 2.0 * u_blur_dir) + blur_tap(v_uv - 2.0 * u_blur_dir)) * 0.1113;
        sum += (blur_tap(v_uv + 3.0 * u_blur_dir) + blur_tap(v_uv - 3.0 * u_blur_dir)) * 0.0366;
        color = sum;
    } else if (mode == 10) {
        vec4 t = texture(u_tex, v_uv);
        vec4 u = unpremul(t);
        u = clamp(u_cm * u + u_cm_offset * step(1e-5, t.a), 0.0, 1.0);
        vec2 muv = vec2(v_uv.x * v_p1.x, 1.0 - (1.0 - v_uv.y) * v_p1.y);
        color = premul_rgba(u) * (u_opacity * texture(u_mask, muv).a);
    } else if (mode == 11) {
        // blend-mode group: Cs' = (1 - ab) Cs + ab B(Cb, Cs), output premultiplied for source-over
        vec4 t = texture(u_tex, v_uv);
        vec4 u = unpremul(t);
        u = clamp(u_cm * u + u_cm_offset * step(1e-5, t.a), 0.0, 1.0);
        vec2 buv = vec2(v_uv.x * v_p1.x, 1.0 - (1.0 - v_uv.y) * v_p1.y);
        vec4 bp = texture(u_backdrop, buv);
        vec3 cb = bp.a > 1e-5 ? bp.rgb / bp.a : vec3(0.0);
        vec3 cs = mix(u.rgb, blend_mode(u_blend, cb, u.rgb), bp.a);
        float a = u.a * u_opacity;
        color = vec4(cs * a, a);
    }

    cov *= clip_coverage();
    if (u_poly_n > 0) cov *= poly_coverage(v_pos);

    if (u_stencil_pass == 1) {
        if (cov < 0.5) discard;
        frag_color = vec4(0.0);
        return;
    }

    frag_color = color * cov;
}

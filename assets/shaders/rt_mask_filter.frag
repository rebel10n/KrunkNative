#version 450 core
// Per-weapon part masks, pass 2 of 2 (client/src/rt/rt_masks.c): PIL's ModeFilter(5) over the class map of
// pass 1 (the most frequent class of the 5x5 box when it is seen more than twice, ties to the lower class,
// else the texel's own; the 2-px border keeps its own), then the UV rect overrides (texel rectangles in the
// target's rows: rt_masks.c mirrors their top-left-origin y), then the class values: R 255 unused,
// G roughness, B metalness (the metallicRoughness layout basic.frag reads).
uniform sampler2D cls_tex;
uniform int rect_count;
uniform int rect_cls[16];
uniform ivec4 rect_px[16]; // x0, y0, x1, y1 (end exclusive)
uniform vec2 values[5];    // per class: G, B in 0..255
out vec4 frag_out;

int class_at(ivec2 p) {
    return int(floor(texelFetch(cls_tex, p, 0).r * 255.0 + 0.5));
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(cls_tex, 0);
    int cls = class_at(p);
    if (p.x >= 2 && p.y >= 2 && p.x < size.x - 2 && p.y < size.y - 2) {
        int n0 = 0;
        int n1 = 0;
        int n2 = 0;
        int n3 = 0;
        int n4 = 0;
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                int k = class_at(p + ivec2(dx, dy));
                n0 += k == 0 ? 1 : 0;
                n1 += k == 1 ? 1 : 0;
                n2 += k == 2 ? 1 : 0;
                n3 += k == 3 ? 1 : 0;
                n4 += k == 4 ? 1 : 0;
            }
        }
        int best = -1;
        int best_n = 2;
        if (n0 > best_n) { best_n = n0; best = 0; }
        if (n1 > best_n) { best_n = n1; best = 1; }
        if (n2 > best_n) { best_n = n2; best = 2; }
        if (n3 > best_n) { best_n = n3; best = 3; }
        if (n4 > best_n) { best_n = n4; best = 4; }
        if (best >= 0) cls = best;
    }
    for (int r = 0; r < 16; r++) {
        if (r >= rect_count) break;
        ivec4 rc = rect_px[r];
        if (p.x >= rc.x && p.y >= rc.y && p.x < rc.z && p.y < rc.w) cls = rect_cls[r];
    }
    vec2 gb = values[clamp(cls, 0, 4)];
    frag_out = vec4(1.0, gb.x / 255.0, gb.y / 255.0, 1.0);
}

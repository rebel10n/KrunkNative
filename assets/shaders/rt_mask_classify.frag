#version 450 core
// Per-weapon part masks, pass 1 of 2 (client/src/rt/rt_masks.c, tools/rt-materials/weapon_masks.py): every
// texel of the weapon's base texture (`src`, texelFetch at the same coordinate: the target has its size) to
// a part class by HSV rules, written as class / 255 in red. The HSV is PIL's (libImaging rgb2hsv: integer
// v, s = int(cr * 255 / max), h = int(fract(h / 6 + 1) * 255)) so the result matches the Python tool's.
// Rules are tried in order, the first match wins; a component < 0 is no test; hue0 > hue1 wraps.
uniform sampler2D src;
uniform int rule_count;
uniform int rule_cls[16];
uniform vec4 rule_hs[16]; // hue0, hue1 (degrees), sat_min, sat_max (exclusive)
uniform vec2 rule_v[16];  // val_min, val_max (exclusive)
uniform int default_cls;  // transparent texels and texels no rule matched
out vec4 frag_out;

void main() {
    ivec4 c = ivec4(floor(texelFetch(src, ivec2(gl_FragCoord.xy), 0) * 255.0 + 0.5));
    int cls = default_cls;
    if (c.a >= 8) {
        int maxc = max(c.r, max(c.g, c.b));
        int minc = min(c.r, min(c.g, c.b));
        int h8 = 0;
        int s8 = 0;
        if (minc != maxc) {
            float cr = float(maxc - minc);
            s8 = int(cr * 255.0 / float(maxc));
            float rc = float(maxc - c.r) / cr;
            float gc = float(maxc - c.g) / cr;
            float bc = float(maxc - c.b) / cr;
            float h;
            if (c.r == maxc) h = bc - gc;
            else if (c.g == maxc) h = 2.0 + rc - bc;
            else h = 4.0 + gc - rc;
            h = h / 6.0 + 1.0;
            h -= floor(h);
            h8 = int(h * 255.0);
        }
        float hue = float(h8) * (360.0 / 255.0);
        float sat = float(s8) / 255.0;
        float val = float(maxc) / 255.0;
        for (int r = 0; r < 16; r++) {
            if (r >= rule_count) break;
            vec4 hs = rule_hs[r];
            vec2 v = rule_v[r];
            bool ok = true;
            if (hs.x >= 0.0) ok = hs.x <= hs.y ? (hue >= hs.x && hue <= hs.y) : (hue >= hs.x || hue <= hs.y);
            if (hs.z >= 0.0 && sat < hs.z) ok = false;
            if (hs.w >= 0.0 && sat >= hs.w) ok = false;
            if (v.x >= 0.0 && val < v.x) ok = false;
            if (v.y >= 0.0 && val >= v.y) ok = false;
            if (ok) {
                cls = rule_cls[r];
                break;
            }
        }
    }
    frag_out = vec4(float(cls) / 255.0, 0.0, 0.0, 1.0);
}

// map-logic WP-B: v7.1.4 data/collisions.js (capsuleColl / triCapsInter) with libs/Capsule.js
// lineLineMinimumPoints and three.js 0.150 Triangle / Plane maths, in doubles like the JS.
// The editor bakes the world-space triangles of "cpx" objects into the map's top-level "tris".
#include <world.h>
#include <math.h>

typedef struct {
    double x, y, z;
} TriVec;

static TriVec tv(const double x, const double y, const double z) { return (TriVec) {x, y, z}; }
static TriVec tv_add(const TriVec a, const TriVec b) { return tv(a.x + b.x, a.y + b.y, a.z + b.z); }
static TriVec tv_sub(const TriVec a, const TriVec b) { return tv(a.x - b.x, a.y - b.y, a.z - b.z); }
static TriVec tv_mul(const TriVec a, const double s) { return tv(a.x * s, a.y * s, a.z * s); }
static double tv_dot(const TriVec a, const TriVec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static TriVec tv_cross(const TriVec a, const TriVec b) { return tv(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static double tv_len(const TriVec a) { return sqrt(tv_dot(a, a)); }
static TriVec tv_norm(const TriVec a) { // Vector3.normalize: divideScalar(length || 1)
    const double l = tv_len(a);
    return tv_mul(a, 1.0 / (l ? l : 1.0));
}

// Triangle.containsPoint (getBarycoord: a degenerate triangle contains nothing)
static int tri_contains(const TriVec a, const TriVec b, const TriVec c, const TriVec p) {
    const TriVec v0 = tv_sub(c, a), v1 = tv_sub(b, a), v2 = tv_sub(p, a);
    const double dot00 = tv_dot(v0, v0), dot01 = tv_dot(v0, v1), dot02 = tv_dot(v0, v2);
    const double dot11 = tv_dot(v1, v1), dot12 = tv_dot(v1, v2);
    const double denom = dot00 * dot11 - dot01 * dot01;
    if (denom == 0) return 0; // target.set(-2, -1, -1)
    const double inv = 1 / denom;
    const double u = (dot11 * dot02 - dot01 * dot12) * inv, v = (dot00 * dot12 - dot01 * dot02) * inv;
    const double bx = 1 - u - v, by = v;
    return bx >= 0 && by >= 0 && bx + by <= 1;
}

// Capsule.lineLineMinimumPoints(line1, line2)
static void line_line_min(const TriVec s1, const TriVec e1, const TriVec s2, const TriVec e2, TriVec *p1, TriVec *p2) {
    const TriVec r = tv_sub(e1, s1), s = tv_sub(e2, s2), w = tv_sub(s2, s1);
    const double a = tv_dot(r, s), b = tv_dot(r, r), c = tv_dot(s, s), d = tv_dot(s, w), e = tv_dot(r, w);
    double t1, t2;
    const double divisor = b * c - a * a;
    if (fabs(divisor) < 1e-10) {
        const double d1 = -d / c, d2 = (a - d) / c;
        if (fabs(d1 - 0.5) < fabs(d2 - 0.5)) {
            t1 = 0;
            t2 = d1;
        } else {
            t1 = 1;
            t2 = d2;
        }
    } else {
        t1 = (d * a + e * c) / divisor;
        t2 = (t1 * a - d) / c;
    }
    t2 = fmax(0, fmin(1, t2));
    t1 = fmax(0, fmin(1, t1));
    *p1 = tv_add(tv_mul(r, t1), s1);
    *p2 = tv_add(tv_mul(s, t2), s2);
}

// triCapsInter(capsule, triangle): the push-out normal and depth, 0 = none
static int tri_caps_inter(const TriVec start, const TriVec end, const double radius, const TriVec a, const TriVec b, const TriVec c,
                          TriVec *normal, double *depth) {
    // Triangle.getPlane: setFromCoplanarPoints(a, b, c): normal (c - b) x (a - b), constant -n.a
    const TriVec n = tv_norm(tv_cross(tv_sub(c, b), tv_sub(a, b)));
    const double constant = -tv_dot(a, n);
    const double d1 = tv_dot(n, start) + constant - radius;
    const double d2 = tv_dot(n, end) + constant - radius;
    if ((d1 > 0 && d2 > 0) || (d1 < -radius && d2 < -radius)) return 0;
    const double delta = fabs(d1 / (fabs(d1) + fabs(d2)));
    const TriVec point = tv_add(start, tv_mul(tv_sub(end, start), delta)); // start.lerp(end, delta)
    if (tri_contains(a, b, c, point)) {
        *normal = n;
        *depth = fabs(fmin(d1, d2));
        return 1;
    }
    const double r2 = radius * radius;
    const TriVec edges[3][2] = {{a, b}, {b, c}, {c, a}};
    for (int i = 0; i < 3; i++) {
        TriVec p1, p2;
        line_line_min(start, end, edges[i][0], edges[i][1], &p1, &p2);
        const TriVec diff = tv_sub(p1, p2);
        if (tv_dot(diff, diff) < r2) {
            *normal = tv_norm(diff);
            *depth = radius - tv_len(diff);
            return 1;
        }
    }
    return 0;
}

int tri_capsule_collide(const float tri[9], const vec3 feet, const float radius, const float height, vec3 *normal, float *depth) {
    // collBounds(tri, x, y, z, r, h): the triangle's AABB against the capsule's
    const double x = feet.x, y = feet.y, z = feet.z, r = radius, h = height;
    const double min_x = fmin(tri[0], fmin(tri[3], tri[6])), max_x = fmax(tri[0], fmax(tri[3], tri[6]));
    const double min_y = fmin(tri[1], fmin(tri[4], tri[7])), max_y = fmax(tri[1], fmax(tri[4], tri[7]));
    const double min_z = fmin(tri[2], fmin(tri[5], tri[8])), max_z = fmax(tri[2], fmax(tri[5], tri[8]));
    if (!(x + r >= min_x && x - r <= max_x && z + r >= min_z && z - r <= max_z && y + h * 2 >= min_y && y - h <= max_y)) return 0;

    const TriVec a = tv(tri[0], tri[1], tri[2]), b = tv(tri[3], tri[4], tri[5]), c = tv(tri[6], tri[7], tri[8]);
    const TriVec start = tv(x, y, z), end = tv(x, y + h, z);
    TriVec n;
    double d;
    if (!tri_caps_inter(start, end, r, a, b, c, &n, &d)) return 0;
    // capsule.translate(normal * depth); {normal: (centre' - centre).normalize(), depth: its length}
    const TriVec move = tv_mul(n, d);
    const double len = tv_len(move);
    const TriVec dir = tv_norm(move);
    *normal = (vec3) {(float) dir.x, (float) dir.y, (float) dir.z};
    *depth = (float) len;
    return 1;
}

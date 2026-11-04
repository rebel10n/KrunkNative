// Cinematic grade (client/src/rt/rt_grade.c, rt.h "Cinematic colour pipeline"): the 3D LUT and a vignette on the
// final display colour. Included by scene_composite.frag and rt_sharpen.frag. grade_on false (the Custom preset,
// rtMode 0) returns the colour untouched.
uniform bool grade_on;
uniform highp sampler3D grade_lut; // RT_GRADE_UNIT
uniform float grade_lut_size;      // RT_GRADE_LUT_SIZE
uniform float grade_vignette;      // RT_GRADE_VIGNETTE
uniform float grade_aspect;        // output width / height

vec3 rt_grade(vec3 color, vec2 uv) {
    if (!grade_on) return color;
    vec3 c = clamp(color, 0.0, 1.0);
    float scale = (grade_lut_size - 1.0) / grade_lut_size, offset = 0.5 / grade_lut_size;
    c = texture(grade_lut, c * scale + offset).rgb;
    vec2 p = (uv - 0.5) * vec2(grade_aspect, 1.0);
    float r = length(p) / length(vec2(grade_aspect, 1.0) * 0.5); // 1 in the corners
    c *= 1.0 - grade_vignette * smoothstep(0.35, 1.0, r * r);
    return c;
}

// Krunker v10.0.4 skin shader 10 "AngelSheel" (deob-10.0.4.js:196020-196071; TSL twin :92370-92420): a translucent shell
// with a fresnel rim, lighter on its back faces. Presets main / accent / melee_main / melee_accent (client/src/skin_fx.c) set
// the vertex warp: uWarp's displacement along normalLocal (:92402-92407) is skin_custom.vert's uDisplace 2.
out vec4 kn_frag_color;
in vec3 vView;

void main() {
    vec3 vPositionW = normalize(vView);
    vec3 vNormalW = normalize(cross(dFdx(vView), dFdy(vView)));
    float fresnelTerm = (0.7 - -min(dot(vPositionW, normalize(vNormalW)), 0.0));
    vec3 fresnel = vec3(1.0, 1.0, 1.0);
    vec3 base = vec3(0.314, 0.671, 0.906);

    if (gl_FrontFacing) {
        kn_frag_color = vec4(base + (fresnel * vec3(fresnelTerm)), 0.9 + (fresnelTerm));
    } else {
        kn_frag_color = vec4(base * 1.5, 0.9);
    }
}

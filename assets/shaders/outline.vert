#version 450 core
// KrunkNative Object Manager selection outline (client/src/om_outline.c): a mesh drawn flat, shifted
// on screen by `offset` (clip units per w: pixels * 2 / viewport) so eight shifted copies around the
// stencil-masked silhouette leave a ring of constant pixel width.
layout (location = 0) in vec3 v_pos;

uniform mat4 transform;
uniform mat4 camera_world_inverse;
uniform mat4 camera_projection;
uniform vec2 offset;

void main() {
    vec4 clip = camera_projection * camera_world_inverse * transform * vec4(v_pos, 1.0);
    clip.xy += offset * clip.w;
    gl_Position = clip;
}

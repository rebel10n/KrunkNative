#version 450 core
// Ray tracing prepass (client/src/rt/rt_pass.c): the opaque world meshes' depth and world
// position at RT resolution, with the camera of the frame's world pass.
layout (location = 0) in vec3 v_pos;
// the geometry's vertex normal (attribute 3, loaders.c geometry_bind_normals: OBJ / glTF models); read only
// for the first-person viewmodel (use_vertex_normal), whose small facets at RT resolution give the
// fragment's derivative normal wrong values along every facet boundary. A VAO without the attribute reads
// the generic (0, 0, 0): the fragment then keeps the derivative normal.
layout (location = 3) in vec3 v_normal;

uniform mat4 transform;
uniform mat4 view_projection;
// motion vectors (phase 6): the same vertex under the previous frame's mesh transform and camera
// (RtInstance.prev_local_to_world, rt_pass.c previous_view_projection); the fragment writes the
// screen-space delta
uniform mat4 prev_transform;
uniform mat4 previous_view_projection;
// TAA (render_target.c): the current camera without its sub-pixel jitter, so the velocity delta
// (cur_clip - prev_clip, both unjittered) carries only real motion
uniform mat4 velocity_view_projection;

out vec3 world_pos;
out vec3 vert_normal;
out vec4 cur_clip;
out vec4 prev_clip;

void main() {
    vec4 world = transform * vec4(v_pos, 1.0);
    world_pos = world.xyz;
    vert_normal = mat3(transform) * v_normal;
    gl_Position = view_projection * world;
    cur_clip = velocity_view_projection * world;
    prev_clip = previous_view_projection * (prev_transform * vec4(v_pos, 1.0));
}

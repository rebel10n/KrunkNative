#version 450 core
// Scene render target composite (client/src/render_target.c): one fullscreen
// triangle from gl_VertexID, no vertex buffers.
out vec2 uv;

void main() {
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    uv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}

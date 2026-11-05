#version 450
// v361 clear quad: one instance per clear rectangle, drawn as a 4-vertex triangle strip. rect = (x0, y0, x1, y1) in NDC of the
// renderer's 4096 x 4096 viewport (x / 2048 - 1: exact for integer pixel edges, so every edge lands on a pixel boundary);
// value = the clear value, passed unchanged (flat: no interpolation) to the fragment shader.
layout(location = 0) in vec4 rect;
layout(location = 1) in vec4 value;
layout(location = 0) flat out vec4 clearValue;
void main() {
    gl_Position = vec4((gl_VertexIndex & 1) != 0 ? rect.z : rect.x, (gl_VertexIndex & 2) != 0 ? rect.w : rect.y, 0.0, 1.0);
    clearValue = value;
}

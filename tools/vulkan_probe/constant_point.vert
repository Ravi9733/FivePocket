#version 450
// Generic native code, independently authored; no game shader payload.
layout(push_constant, std430) uniform ConstantPoint {
    layout(offset=0) vec4 position;
    layout(offset=16) vec4 parameter;
} values;
layout(location=0) out vec4 parameter0;
void main() {
    gl_Position = values.position;
    gl_PointSize = 1.0;
    parameter0 = values.parameter;
}

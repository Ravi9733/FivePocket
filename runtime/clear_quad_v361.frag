#version 450
// v361 clear quad: the clear value to every colour output; the pipeline's colour write masks select the one cleared attachment
// (blending off, every other attachment's mask 0, depth and stencil tests off).
layout(location = 0) flat in vec4 clearValue;
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
void main() {
    o0 = clearValue;
    o1 = clearValue;
    o2 = clearValue;
    o3 = clearValue;
}

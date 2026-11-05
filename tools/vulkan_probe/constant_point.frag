#version 450
// Generic location-0 linkage for the native constant-copy profile only.
layout(location=0) in vec4 parameter0;
layout(location=0) out vec4 color0;
void main() { color0 = parameter0; }

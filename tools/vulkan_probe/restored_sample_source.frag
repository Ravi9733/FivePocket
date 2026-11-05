#version 450
// Diagnostic fixture: restore caller-provided linear float4 values for exactly
// one sample per pixel. No private pixels, game admission or production use.
layout(std430,binding=0) readonly buffer Input {uint words[];} inputData;
layout(location=0) out vec4 color;
void main() {
    uint at=2052u+(uint(gl_FragCoord.y)*inputData.words[1]+uint(gl_FragCoord.x))*4u;
    color=uintBitsToFloat(uvec4(inputData.words[at],inputData.words[at+1u],inputData.words[at+2u],inputData.words[at+3u]));
    gl_SampleMask[0]=int(1u<<inputData.words[0]);
}

#version 450
// AUTHORED test shader, not translated guest microcode. Consume the same
// big-endian vertex buffer layout used by the production ABI25 tile entry.
layout(set=0,binding=0,std430) readonly buffer Input { uint words[]; } source;
uint swap32(uint x) {
    return ((x&255u)<<24)|((x&65280u)<<8)|((x>>8)&65280u)|(x>>24);
}
void main() {
    uint at=2052u+uint(gl_VertexIndex)*9u;
    gl_Position=vec4(uintBitsToFloat(swap32(source.words[at])),
                     uintBitsToFloat(swap32(source.words[at+1u])),
                     uintBitsToFloat(swap32(source.words[at+2u])),1.0);
}

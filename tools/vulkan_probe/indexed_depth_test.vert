#version 450
layout(set=0,binding=0,std430) readonly buffer Input { uint words[]; } source;
layout(location=0) out vec4 vertexColor;
uint be(uint w) { return (w<<24)|((w&0xff00u)<<8)|((w>>8)&0xff00u)|(w>>24); }
vec4 readVector(uint at) {
    return vec4(uintBitsToFloat(be(source.words[at])),uintBitsToFloat(be(source.words[at+1])),
                uintBitsToFloat(be(source.words[at+2])),uintBitsToFloat(be(source.words[at+3])));
}
void main() {
    uint at=2052u+uint(gl_VertexIndex)*8u;
    gl_Position=readVector(at);
    vertexColor=readVector(at+4);
}

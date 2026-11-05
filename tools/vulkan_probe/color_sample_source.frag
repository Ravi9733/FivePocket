#version 450
// Authored varying input for storage conversion tests, not a game shader.
layout(std430,binding=0) readonly buffer Input {uint words[];} inputData;
layout(location=0) out vec4 color;
void main() {
    uint n=uint(gl_FragCoord.x)+uint(gl_FragCoord.y)*256u;
    if(inputData.words[0]==0u) {
        float f=float(n)/65536.0;
        color=vec4(f,1.0-f,f*2.0-0.5,f);
    } else if(inputData.words[0]==2u) {
        color=vec4(0.125,0.25,0.5,0.75);
    } else {
        uint at=4u+(n%inputData.words[1])*4u;
        color=uintBitsToFloat(uvec4(inputData.words[at],inputData.words[at+1u],inputData.words[at+2u],inputData.words[at+3u]));
    }
}

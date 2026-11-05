#version 450
layout(set=0,binding=0,std430) readonly buffer Input { uint words[]; } source;
void main() {
    vec2 size=vec2(source.words[2],source.words[3]);
    vec2 p=gl_VertexIndex==0?vec2(0):gl_VertexIndex==1?vec2(size.x*2,0):vec2(0,size.y*2);
    precise float z=uintBitsToFloat(source.words[0])+p.x*uintBitsToFloat(source.words[1]);
    gl_Position=vec4(p/vec2(1024,512)-1,z,1);
}

#version 450
layout(set=0,binding=1) uniform sampler2D t0;
layout(set=0,binding=2) uniform sampler2D t1;
layout(set=0,binding=3) uniform sampler2D t2;
layout(set=0,binding=4) uniform sampler2D t3;
layout(location=0) out vec4 color;
void main() {
    color=vec4(texture(t0,vec2(.5)).r,texture(t1,vec2(.5)).g,
               texture(t2,vec2(.5)).b,texture(t3,vec2(.5)).a);
}

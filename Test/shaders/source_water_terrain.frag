#version 410
layout(location = 0) in vec3 WorldPosition;
layout(location = 1) in vec2 UV;
layout(location = 2) in vec3 Normal;
layout(location = 0) out vec4 FragColor;
void main()
{
    float light=0.43+0.57*max(0.0,dot(normalize(Normal),normalize(vec3(-0.4,0.82,-0.35))));
    FragColor=vec4(vec3(0.30,0.36,0.16)*light,1.0);
}

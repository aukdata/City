#version 410
layout(location = 0) in vec3 WorldPosition;
layout(location = 1) in vec2 UV;
layout(location = 2) in vec3 Normal;
layout(location = 0) out vec4 FragColor;
layout(std140) uniform PSPerFrame { vec3 ambientColor; vec3 sunColor; vec3 sunDirection; };
layout(std140) uniform PSPerView { vec3 eyePosition; };
layout(std140) uniform WaterVisualParams { vec4 parameters; };
float hash(vec2 p) { return fract(sin(dot(p,vec2(127.1,311.7))) * 43758.5453); }
float noise(vec2 p) { vec2 i=floor(p),f=fract(p); f=f*f*(3.0-2.0*f); return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y); }
void main()
{
    float depth=max(UV.x,0.0);
    if(depth<0.002) discard;
    vec2 p=WorldPosition.xz;
    float t=parameters.x;
    vec3 baseNormal=normalize(Normal);
    vec2 downhill=baseNormal.xz;
    float slope=length(downhill);
    vec2 flow=UV.y>4.0 ? vec2(cos(UV.y-8.0),sin(UV.y-8.0)) : slope>0.002 ? downhill/slope : vec2(0.8,0.6);
    vec2 across=vec2(-flow.y,flow.x);
    float river=UV.y>4.0 ? 1.0 : smoothstep(0.006,0.05,slope);
    float speed=max(length(Normal)-1.0,0.0);
    float along=dot(p,flow), side=dot(p,across);
    // Broad, low-amplitude sea waves avoid the old crossing checker pattern.
    float swell=sin(along*0.023-t*0.45)*0.024;
    float ripple=cos(along*0.083+side*0.027-t*0.85)*0.009;
    float stream=sin(along*0.38-t*speed*22.8+sin(side*0.12))*0.012*river;
    float detailFade=1.0/(1.0+length(fwidth(p))*0.2);
    vec2 perturbation=(flow*(swell+ripple+stream)+across*cos(side*0.061+t*0.38)*0.008)*detailFade;
    vec3 n=normalize(baseNormal+vec3(perturbation.x,0.0,perturbation.y));
    vec3 v=normalize(eyePosition-WorldPosition);
    float fresnel=0.025+0.975*pow(1.0-max(dot(v,n),0.0),5.0);
    vec3 reflected=reflect(-v,n);
    vec3 sky=mix(vec3(0.68,0.77,0.81),vec3(0.18,0.36,0.52),clamp(reflected.y,0.0,1.0));
    float cloud=noise(reflected.xz*3.0+vec2(t*0.003,0.0));
    sky=mix(sky,vec3(0.81,0.86,0.87),smoothstep(0.55,0.82,cloud)*0.25);
    vec3 transmittance=exp(-vec3(0.62,0.22,0.13)*depth);
    vec3 waterColor=mix(vec3(0.015,0.10,0.14),vec3(0.16,0.32,0.27),transmittance);
    vec3 sun=normalize(sunDirection);
    vec3 h=normalize(v+sun);
    float specular=pow(max(dot(n,h),0.0),90.0)*0.55;
    vec3 color=mix(waterColor,sky,clamp(fresnel+0.06,0.0,1.0))+vec3(1.0,0.95,0.82)*specular;
    float streak=pow(clamp(noise(vec2(along*0.10-t*speed*6.0,side*0.8))-0.35,0.0,1.0)*1.5,3.0)*river;
    color+=vec3(0.11,0.15,0.14)*streak;
    // Millimetre bank clipping stays signed; centimetre films retain a visible reflection.
    float alpha=(0.24+0.56*(1.0-exp(-depth*1.5))+fresnel*0.18)*smoothstep(0.002,0.008,depth);
    FragColor=vec4(color*(ambientColor+sunColor*0.6),clamp(alpha,0.0,0.96));
}

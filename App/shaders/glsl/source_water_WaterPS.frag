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
// The paired samples elongate broad surface detail in the downstream direction.
vec2 streamDetail(vec2 q,vec2 flow)
{
    float upstream=noise(q+flow*0.85);
    float downstream=noise(q-flow*0.85);
    return vec2((upstream+downstream)*0.5,upstream-downstream);
}
void main()
{
    float depth=max(UV.x,0.0);
    float edgeWidth=max(0.04,fwidth(UV.x)*1.15);
    if(depth<0.002) discard;
    vec2 p=WorldPosition.xz;
    float t=parameters.x;
    float river=UV.y>4.0 ? 1.0 : 0.0;
    float flowLength=length(Normal.xz);
    vec2 flow=flowLength>0.001 ? Normal.xz/flowLength : vec2(0.8,0.6);
    vec2 across=vec2(-flow.y,flow.x);
    float speed=river*clamp(Normal.y-1.0,0.0,4.0);
    vec3 baseNormal=river>0.5 ? vec3(0.0,1.0,0.0) : normalize(Normal);

    vec2 detail=vec2(0.5,0.0);
    if(river>0.5)
    {
        // Visual time is in game minutes; river speeds are metres per physical second.
        float flowTime=t*60.0;
        // Two bounded phases hide each reset and prevent long-running flow gradients from stretching the surface.
        float phase0=fract(flowTime/24.0);
        float phase1=fract(phase0+0.5);
        float phaseBlend=smoothstep(0.0,1.0,abs(phase0*2.0-1.0));
        vec2 q0=(p-flow*(phase0*24.0)*speed)*0.08;
        vec2 q1=(p-flow*(phase1*24.0)*speed)*0.08;
        detail=mix(streamDetail(q0,flow),streamDetail(q1,flow),phaseBlend);
    }
    float ribbon=detail.x;
    vec2 current=flow*(ribbon-0.5)*0.012+across*detail.y*0.006;
    float seaAlong=dot(p,vec2(0.8,0.6));
    float seaSide=dot(p,vec2(-0.6,0.8));
    float swell=sin(seaAlong*0.023-t*0.45)*0.014;
    float ripple=cos(seaAlong*0.083+seaSide*0.027-t*0.85)*0.006;
    vec2 seaWave=vec2(0.8,0.6)*(swell+ripple);
    float detailFade=1.0/(1.0+length(fwidth(p))*0.2);
    vec2 perturbation=mix(seaWave,current,river)*detailFade;
    vec3 n=normalize(baseNormal+vec3(perturbation.x,0.0,perturbation.y));
    vec3 v=normalize(eyePosition-WorldPosition);
    float fresnel=0.025+0.975*pow(1.0-max(dot(v,n),0.0),5.0);
    vec3 reflected=reflect(-v,n);
    vec3 sky=mix(vec3(0.60,0.70,0.73),vec3(0.20,0.36,0.47),clamp(reflected.y,0.0,1.0));

    // Visible riverbed warms the shallows; deeper water settles into subdued blue-green.
    vec3 transmittance=exp(-vec3(0.90,0.65,0.48)*depth);
    vec3 waterColor=mix(vec3(0.025,0.12,0.15),vec3(0.30,0.34,0.23),transmittance);
    vec3 sun=normalize(sunDirection);
    vec3 h=normalize(v+sun);
    float specular=pow(max(dot(n,h),0.0),64.0)*mix(0.12,0.075,river);
    vec3 color=mix(waterColor,sky,clamp(0.03+fresnel*0.80,0.0,0.85))+vec3(1.0,0.95,0.82)*specular;
    float streak=smoothstep(0.54,0.76,ribbon)*river*smoothstep(0.08,0.6,depth)*smoothstep(0.02,0.5,speed)*detailFade;
    color+=vec3(0.012,0.023,0.021)*streak;
    // Fade inward from the signed shoreline instead of painting an opaque rim over dry ground.
    float alpha=(0.15+0.68*(1.0-exp(-depth*0.9))+fresnel*0.10)*smoothstep(0.002,0.002+edgeWidth,depth);
    FragColor=vec4(color*(ambientColor+sunColor*0.6),clamp(alpha,0.0,0.94));
}

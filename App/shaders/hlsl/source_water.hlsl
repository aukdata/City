struct PSInput { float4 position:SV_POSITION; float3 WorldPosition:TEXCOORD0; float2 UV:TEXCOORD1; float3 Normal:TEXCOORD2; };
cbuffer PSPerFrame:register(b0) { float3 ambientColor; float3 sunColor; float3 sunDirection; };
cbuffer PSPerView:register(b1) { float3 eyePosition; };
cbuffer WaterVisualParams:register(b5) { float4 parameters; };
float hash(float2 p) { return frac(sin(dot(p,float2(127.1,311.7))) * 43758.5453); }
float noise(float2 p) { float2 i=floor(p),f=frac(p); f=f*f*(3.0-2.0*f); return lerp(lerp(hash(i),hash(i+float2(1,0)),f.x),lerp(hash(i+float2(0,1)),hash(i+float2(1,1)),f.x),f.y); }
float4 WaterPS(PSInput input):SV_TARGET
{
    float depth=max(input.UV.x,0.0);
    if(depth<0.002) discard;
    float2 p=input.WorldPosition.xz;
    float t=parameters.x;
    float3 baseNormal=normalize(input.Normal);
    float2 downhill=baseNormal.xz;
    float slope=length(downhill);
    float2 flow=input.UV.y>4 ? float2(cos(input.UV.y-8),sin(input.UV.y-8)) : slope>0.002 ? downhill/slope : float2(0.8,0.6);
    float2 across=float2(-flow.y,flow.x);
    float river=input.UV.y>4 ? 1 : smoothstep(0.006,0.05,slope);
    float speed=max(length(input.Normal)-1.0,0.0);
    float along=dot(p,flow), side=dot(p,across);
    // Broad, low-amplitude sea waves avoid the old crossing checker pattern.
    float swell=sin(along*0.023-t*0.45)*0.024;
    float ripple=cos(along*0.083+side*0.027-t*0.85)*0.009;
    float stream=sin(along*0.38-t*speed*22.8+sin(side*0.12))*0.012*river;
    float detailFade=1.0/(1.0+length(fwidth(p))*0.2);
    float2 perturbation=(flow*(swell+ripple+stream)+across*cos(side*0.061+t*0.38)*0.008)*detailFade;
    float3 n=normalize(baseNormal+float3(perturbation.x,0.0,perturbation.y));
    float3 v=normalize(eyePosition-input.WorldPosition);
    float fresnel=0.025+0.975*pow(1.0-max(dot(v,n),0.0),5.0);
    float3 reflected=reflect(-v,n);
    float3 sky=lerp(float3(0.68,0.77,0.81),float3(0.18,0.36,0.52),clamp(reflected.y,0.0,1.0));
    float cloud=noise(reflected.xz*3.0+float2(t*0.003,0.0));
    sky=lerp(sky,float3(0.81,0.86,0.87),smoothstep(0.55,0.82,cloud)*0.25);
    float3 transmittance=exp(-float3(0.62,0.22,0.13)*depth);
    float3 waterColor=lerp(float3(0.015,0.10,0.14),float3(0.16,0.32,0.27),transmittance);
    float3 sun=normalize(sunDirection);
    float3 h=normalize(v+sun);
    float specular=pow(max(dot(n,h),0.0),90.0)*0.55;
    float3 color=lerp(waterColor,sky,clamp(fresnel+0.06,0.0,1.0))+float3(1.0,0.95,0.82)*specular;
    float streak=pow(saturate(noise(float2(along*0.10-t*speed*6.0,side*0.8))-0.35)*1.5,3.0)*river;
    color+=float3(0.11,0.15,0.14)*streak;
    // Millimetre bank clipping stays signed; centimetre films retain a visible reflection.
    float alpha=(0.24+0.56*(1.0-exp(-depth*1.5))+fresnel*0.18)*smoothstep(0.002,0.008,depth);
    return float4(color*(ambientColor+sunColor*0.6),clamp(alpha,0.0,0.96));
}

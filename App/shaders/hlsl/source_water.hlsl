struct PSInput { float4 position:SV_POSITION; float3 WorldPosition:TEXCOORD0; float2 UV:TEXCOORD1; float3 Normal:TEXCOORD2; };
cbuffer PSPerFrame:register(b0) { float3 ambientColor; float3 sunColor; float3 sunDirection; };
cbuffer PSPerView:register(b1) { float3 eyePosition; };
cbuffer WaterVisualParams:register(b5) { float4 parameters; };
float hash(float2 p) { return frac(sin(dot(p,float2(127.1,311.7))) * 43758.5453); }
float noise(float2 p) { float2 i=floor(p),f=frac(p); f=f*f*(3.0-2.0*f); return lerp(lerp(hash(i),hash(i+float2(1,0)),f.x),lerp(hash(i+float2(0,1)),hash(i+float2(1,1)),f.x),f.y); }
// The paired samples elongate broad surface detail in the downstream direction.
float2 streamDetail(float2 q,float2 flow)
{
    float upstream=noise(q+flow*0.85);
    float downstream=noise(q-flow*0.85);
    return float2((upstream+downstream)*0.5,upstream-downstream);
}
float4 WaterPS(PSInput input):SV_TARGET
{
    float depth=max(input.UV.x,0.0);
    float edgeWidth=max(0.04,fwidth(input.UV.x)*1.15);
    if(depth<0.002) discard;
    float2 p=input.WorldPosition.xz;
    float t=parameters.x;
    float river=input.UV.y>4.0 ? 1.0 : 0.0;
    float flowLength=length(input.Normal.xz);
    float2 flow=flowLength>0.001 ? input.Normal.xz/flowLength : float2(0.8,0.6);
    float2 across=float2(-flow.y,flow.x);
    float speed=river*clamp(input.Normal.y-1.0,0.0,4.0);
    float3 baseNormal=river>0.5 ? float3(0.0,1.0,0.0) : normalize(input.Normal);

    float2 detail=float2(0.5,0.0);
    if(river>0.5)
    {
        // Visual time is in game minutes; river speeds are metres per physical second.
        float flowTime=t*60.0;
        // Two bounded phases hide each reset and prevent long-running flow gradients from stretching the surface.
        float phase0=frac(flowTime/24.0);
        float phase1=frac(phase0+0.5);
        float phaseBlend=smoothstep(0.0,1.0,abs(phase0*2.0-1.0));
        float2 q0=(p-flow*(phase0*24.0)*speed)*0.08;
        float2 q1=(p-flow*(phase1*24.0)*speed)*0.08;
        detail=lerp(streamDetail(q0,flow),streamDetail(q1,flow),phaseBlend);
    }
    float ribbon=detail.x;
    float2 current=flow*(ribbon-0.5)*0.012+across*detail.y*0.006;
    float seaAlong=dot(p,float2(0.8,0.6));
    float seaSide=dot(p,float2(-0.6,0.8));
    float swell=sin(seaAlong*0.023-t*0.45)*0.014;
    float ripple=cos(seaAlong*0.083+seaSide*0.027-t*0.85)*0.006;
    float2 seaWave=float2(0.8,0.6)*(swell+ripple);
    float detailFade=1.0/(1.0+length(fwidth(p))*0.2);
    float2 perturbation=lerp(seaWave,current,river)*detailFade;
    float3 n=normalize(baseNormal+float3(perturbation.x,0.0,perturbation.y));
    float3 v=normalize(eyePosition-input.WorldPosition);
    float fresnel=0.025+0.975*pow(1.0-max(dot(v,n),0.0),5.0);
    float3 reflected=reflect(-v,n);
    float3 sky=lerp(float3(0.60,0.70,0.73),float3(0.20,0.36,0.47),clamp(reflected.y,0.0,1.0));

    // Visible riverbed warms the shallows; deeper water settles into subdued blue-green.
    float3 transmittance=exp(-float3(0.90,0.65,0.48)*depth);
    float3 waterColor=lerp(float3(0.025,0.12,0.15),float3(0.30,0.34,0.23),transmittance);
    float3 sun=normalize(sunDirection);
    float3 h=normalize(v+sun);
    float specular=pow(max(dot(n,h),0.0),64.0)*lerp(0.12,0.075,river);
    float3 color=lerp(waterColor,sky,clamp(0.03+fresnel*0.80,0.0,0.85))+float3(1.0,0.95,0.82)*specular;
    float streak=smoothstep(0.54,0.76,ribbon)*river*smoothstep(0.08,0.6,depth)*smoothstep(0.02,0.5,speed)*detailFade;
    color+=float3(0.012,0.023,0.021)*streak;
    // Fade inward from the signed shoreline instead of painting an opaque rim over dry ground.
    float alpha=(0.15+0.68*(1.0-exp(-depth*0.9))+fresnel*0.10)*smoothstep(0.002,0.002+edgeWidth,depth);
    return float4(color*(ambientColor+sunColor*0.6),clamp(alpha,0.0,0.94));
}

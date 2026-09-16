//-----------------------------------------------
//
//	This file is part of the Siv3D Engine.
//
//	Copyright (c) 2008-2025 Ryo Suzuki
//	Copyright (c) 2016-2025 OpenSiv3D Project
//
//	Licensed under the MIT License.
//
//-----------------------------------------------

//
//	Textures
//
Texture2D		g_texture0 : register(t0);
SamplerState	g_sampler0 : register(s0);

namespace s3d
{
	//
	//	VS Input
	//
	struct VSInput
	{
		float4 position : POSITION;
		float3 normal : NORMAL;
		float2 uv : TEXCOORD0;
	};

	//
	//	VS Output / PS Input
	//
	struct PSInput
	{
		float4 position : SV_POSITION;
		float3 worldPosition : TEXCOORD0;
		float2 uv : TEXCOORD1;
		float3 normal : TEXCOORD2;
	};
}

//
//	Constant Buffer
//
cbuffer VSPerView : register(b1)
{
	row_major float4x4 g_worldToProjected;
}

cbuffer VSPerObject : register(b2)
{
	row_major float4x4 g_localToWorld;
}

cbuffer VSPerMaterial : register(b3)
{
	float4 g_uvTransform;
}

cbuffer PSPerFrame : register(b0)
{
	float3 g_globalAmbientColor;
	float3 g_sunColor;
	float3 g_sunDirection;
}

cbuffer PSPerView : register(b1)
{
	float3 g_eyePosition;
}

cbuffer PSPerMaterial : register(b3)
{
	float3 g_ambientColor;
	uint   g_hasTexture;
	float4 g_diffuseColor;
	float3 g_specularColor;
	float  g_shininess;
	float3 g_emissionColor;
}

//
//	Functions
//
s3d::PSInput VS(s3d::VSInput input)
{
	s3d::PSInput result;

	const float4 worldPosition = mul(input.position, g_localToWorld);

	result.position			= mul(worldPosition, g_worldToProjected);
	result.worldPosition	= worldPosition.xyz;
	result.uv				= (input.uv * g_uvTransform.xy + g_uvTransform.zw);
	result.normal			= mul(input.normal, (float3x3)g_localToWorld);
	return result;
}

float4 GetDiffuseColor(float2 uv)
{
	float4 diffuseColor = g_diffuseColor;

	if (g_hasTexture)
	{
		diffuseColor *= g_texture0.Sample(g_sampler0, uv);
	}

	return diffuseColor;
}

float3 CalculateDiffuseReflection(float3 n, float3 l, float3 lightColor, float3 diffuseColor, float3 ambientColor)
{
	const float3 directColor = lightColor * saturate(dot(n, l));
	return ((ambientColor + directColor) * diffuseColor);
}

float3 CalculateSpecularReflection(float3 n, float3 h, float shininess, float nl, float3 lightColor, float3 specularColor)
{
	const float highlight = pow(saturate(dot(n, h)), shininess) * float(0.0 < nl);
	return (lightColor * specularColor * highlight);
}

// City lighting extensions: manual PCF with Reverse Z and continuous terrain variation.
Texture2D g_shadowMap : register(t1);
Texture2D g_coastalSand : register(t2);
Texture2D g_dynamicShadowMap : register(t3);
SamplerState g_shadowSampler : register(s1);
cbuffer CityParameters : register(b4)
{
	row_major float4x4 g_worldToShadow;
	float4 g_shadowParameters;
	float4 g_fogColorDensity;
	float4 g_dynamicShadow;
	float4 g_altitudeBands;
	float4 g_terrainVariation;
}

float Depth_PS(s3d::PSInput input) : SV_TARGET
{
	return input.position.z;
}

float shadowVisibility(float3 position, float3 normal)
{
	if (g_shadowParameters.w <= 0) return 1;
	float4 projected = mul(float4(position, 1), g_worldToShadow);
	float3 ndc = projected.xyz / projected.w;
	float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
	if (any(uv < 0.002) || any(uv > 0.998) || ndc.z < 0 || ndc.z > 1)
		return 1;
	float bias = g_shadowParameters.z * (1 + 3 * (1 - saturate(dot(normal, g_sunDirection))));
	float lit = 0;
	[unroll] for (int y = -1; y <= 1; ++y)
	[unroll] for (int x = -1; x <= 1; ++x)
	{
		float depth = g_shadowMap.SampleLevel(g_shadowSampler, uv + float2(x, y) * g_shadowParameters.xy, 0).r;
		if (g_dynamicShadow.x > 0.5)
		{
			depth = max(depth, g_dynamicShadowMap.SampleLevel(g_shadowSampler, uv + float2(x, y) * g_shadowParameters.xy, 0).r);
		}
		lit += (ndc.z + bias >= depth) ? 1 : 0;
	}
	float border = saturate(min(min(uv.x, uv.y), min(1 - uv.x, 1 - uv.y)) * 24);
	return lerp(1, lit / 9, g_shadowParameters.w * border);
}

float4 shadeCity(s3d::PSInput input, float4 albedo)
{
	float3 normal = normalize(input.normal);
	float visibility = shadowVisibility(input.worldPosition, normal);
	float3 sunlight = g_sunColor * visibility;
	float3 ambient = g_globalAmbientColor * lerp(float3(0.58, 0.54, 0.48), float3(0.88, 0.95, 1.06), saturate(normal.y * 0.5 + 0.5));
	float3 diffuse = CalculateDiffuseReflection(normal, g_sunDirection, sunlight, albedo.rgb, ambient);
	float3 view = normalize(g_eyePosition - input.worldPosition);
	float3 halfVector = normalize(view + g_sunDirection);
	float3 specular = CalculateSpecularReflection(normal, halfVector, max(8, g_shininess), dot(normal, g_sunDirection), sunlight, g_specularColor * 0.35);
	float3 color = diffuse + specular + g_emissionColor;
	float transmittance = exp(-g_fogColorDensity.w * distance(g_eyePosition, input.worldPosition));
	return float4(lerp(g_fogColorDensity.rgb, color, transmittance), albedo.a);
}

float4 Shading_PS(s3d::PSInput input) : SV_TARGET
{
	return shadeCity(input, GetDiffuseColor(input.uv));
}

float4 Terrain_PS(s3d::PSInput input) : SV_TARGET
{
	float4 albedo = GetDiffuseColor(input.uv);
	if (g_hasTexture)
	{
		float2 rotatedUv = float2(-input.uv.y, input.uv.x) * 0.173 + float2(0.31, 0.67);
		float3 broad = g_texture0.Sample(g_sampler0, rotatedUv).rgb;
		albedo.rgb *= lerp(float3(0.80, 0.84, 0.77), float3(1.12, 1.10, 1.04), saturate(broad * 2));
	}
	float3 sand = g_coastalSand.Sample(g_sampler0, input.uv).rgb * float3(0.76, 0.70, 0.55);
	float dryLand = smoothstep(0.25, 3.0, input.worldPosition.y);
	albedo.rgb = lerp(sand, albedo.rgb, dryLand);
	float2 ground = input.worldPosition.xz;
	float variation = sin(ground.x * .008 + sin(ground.y * .006)) * sin(ground.y * .009);
	float altitude = input.worldPosition.y + variation * g_terrainVariation.x;
	float alpine = smoothstep(g_altitudeBands.x, g_altitudeBands.y, altitude);
	float steepness = 1 - saturate(normalize(input.normal).y);
	float mineral = dot(albedo.rgb, float3(.2126,.7152,.0722));
	float3 alpineCover = lerp(float3(.25,.29,.16),float3(.35,.34,.32),saturate(steepness * 2 + alpine * .5));
	albedo.rgb = lerp(albedo.rgb,alpineCover * (.8 + mineral),alpine);
	float snow = smoothstep(g_altitudeBands.z, g_altitudeBands.w, altitude);
	float3 snowColor = float3(.86,.9,.95) * (1 + variation * .035);
	albedo.rgb = lerp(albedo.rgb,snowColor,snow * (1 - smoothstep(.55,.85,steepness)));
	return shadeCity(input, albedo);
}

// Construction soil and crushed aggregate: two rotated samples suppress visible
// repetition, while the normal photograph supplies fine mineral relief.
Texture2D g_earthNormal : register(t4);
float4 constructionSurface(s3d::PSInput input, bool aggregate)
{
	float2 uv = input.uv;
	float2 rotated = float2(uv.x * .8 - uv.y * .6, uv.x * .6 + uv.y * .8) * 1.317 + float2(.371, .829);
	float3 first = g_texture0.Sample(g_sampler0, uv).rgb;
	float3 second = g_texture0.Sample(g_sampler0, rotated).rgb;
	float3 albedo = (first + second) * .5;
	float2 position = input.worldPosition.xz;
	float broad = sin(position.x * .19 + sin(position.y * .14)) * sin(position.y * .23 + position.x * .09);
	albedo *= 1 + broad * .07;
	if (aggregate)
	{
		float grey = dot(albedo, float3(.2126, .7152, .0722));
		albedo = lerp(albedo, grey.xxx * float3(.94, .98, 1.02), .88);
	}
	float3 mineral = g_earthNormal.Sample(g_sampler0, uv).xyz * 2 - 1;
	float3 n = normalize(input.normal);
	input.normal = normalize(n + float3(mineral.x, 0, mineral.y) * .22 * saturate(n.y));
	return shadeCity(input, float4(albedo, 1) * g_diffuseColor);
}
float4 Earth_PS(s3d::PSInput input) : SV_TARGET { return constructionSurface(input, false); }
float4 Aggregate_PS(s3d::PSInput input) : SV_TARGET { return constructionSurface(input, true); }

// Metre-space UVs follow each parcel's cultivation axis. Filter rows with screen
// derivatives so subpixel crops turn into a stable average, without moire.
float fieldNoise(float2 p)
{
	return frac(sin(dot(p,float2(127.1,311.7)))*43758.5453);
}
float4 cultivatedSurface(s3d::PSInput input,bool paddy)
{
	float2 uv=input.uv;
	float spacing=paddy ? .30 : .85;
	float phase=uv.x/spacing;
	float footprint=max(fwidth(phase),fwidth(uv.y/spacing));
	float resolved=1-smoothstep(.2,1.2,footprint);
	float ridge=cos(phase*6.2831853);
	float crop=smoothstep(.45,.9,ridge);
	float2 detailUv=uv/(paddy ? .3 : .18);
	float detailWeight=1-smoothstep(.3,1.0,max(fwidth(detailUv.x),fwidth(detailUv.y)));
	float variation=lerp(.5,fieldNoise(floor(detailUv)),detailWeight);
	float3 soil=g_hasTexture ? g_texture0.Sample(g_sampler0,uv*.21).rgb : float3(.35,.3,.2);
	soil=lerp(float3(.10,.059,.031),float3(.21,.125,.060),saturate(dot(soil,float3(.3,.5,.2))*2));
	soil*=lerp(1, .75+.32*(ridge*.5+.5),resolved);
	float3 green=lerp(float3(.075,.13,.022),float3(.19,.25,.045),variation);
	float3 albedo;
	if (paddy)
	{
		float along=cos(uv.y/.30*6.2831853);
		float rice=crop*smoothstep(-.25,.65,along);
		float3 water=float3(.045,.068,.052)+sin(uv.y*2.4+sin(uv.x*.7))*.004;
		float fresnel=pow(1-saturate(dot(normalize(g_eyePosition-input.worldPosition),normalize(input.normal))),4);
		water=lerp(water,float3(.23,.29,.31),fresnel*.65);
		albedo=lerp(water,green,lerp(.35,rice,resolved));
	}
	else
	{
		// Intermittent planting leaves exposed soil between cultivated rows.
		crop*=smoothstep(-.6,.2,cos(uv.y*7.4));
		albedo=lerp(soil,green,lerp(.24,crop*.86,resolved));
	}
	float3 across=ddx(input.worldPosition)*ddy(uv.y)-ddy(input.worldPosition)*ddx(uv.y);
	across/=max(length(across),.000001);
	input.normal=normalize(input.normal+across*sin(phase*6.2831853)*(paddy ? .12 : .48)*resolved);
	return shadeCity(input,float4(albedo,1));
}
float4 Field_PS(s3d::PSInput input) : SV_TARGET { return cultivatedSurface(input,false); }
float4 Paddy_PS(s3d::PSInput input) : SV_TARGET { return cultivatedSurface(input,true); }
float4 Foliage_PS(s3d::PSInput input) : SV_TARGET
{
	float3 position=input.worldPosition*18;
	float detail=1-smoothstep(.6,2.0,length(fwidth(position)));
	float leaf=fieldNoise(floor(position.xz+position.y*.43));
	float broad=sin(input.worldPosition.x*.73+sin(input.worldPosition.z*.91))*cos(input.worldPosition.y*1.37);
	float3 color=g_diffuseColor.rgb*lerp(.80,1.10,saturate(.45+broad*.3))*lerp(1,.58+leaf*.84,detail);
	float3 normal=normalize(input.normal+float3(sin(position.x),0,cos(position.z))*.18*detail);
	input.normal=normal;
	return shadeCity(input,float4(color,1));
}


float4 River_PS(s3d::PSInput input) : SV_TARGET
{
    float2 p = input.worldPosition.xz;
    float detail = 1 - smoothstep(.15, 1.6, length(fwidth(p)));
    float ripple = sin(p.x * 1.9 + sin(p.y * .37)) * cos(p.y * 2.4);
    input.normal = normalize(input.normal + float3(ripple, 0, cos(p.x * .83 + p.y * 2.2)) * .065 * detail);
    float fresnel = pow(1 - saturate(dot(normalize(g_eyePosition - input.worldPosition), input.normal)), 4);
    float3 water = lerp(float3(.028,.115,.13), float3(.28,.42,.49), fresnel * .75);
    water += ripple * .007 * detail;
    return shadeCity(input, float4(water, 1));
}


// Sidewalk coordinates are lateral distance and curve arc length in metres.
float4 Pavement_PS(s3d::PSInput input) : SV_TARGET
{
	float2 p = input.uv / float2(.40,.60);
	p.y += fmod(floor(p.x),2) * .5;
	float2 edge = min(frac(p),1-frac(p));
	float2 footprint = max(fwidth(p),.0001);
	float2 seams = 1 - smoothstep(.008,.008+footprint,edge);
	float seam = max(seams.x,seams.y);
	float resolved = 1-smoothstep(.2,1.0,max(footprint.x,footprint.y));
	float tile = fieldNoise(floor(p));
	float3 albedo = g_diffuseColor.rgb * lerp(.88,1.12,tile);
	albedo *= 1-seam*.36*resolved;
	float grain = fieldNoise(floor(input.worldPosition.xz*160));
	albedo *= lerp(1,.93+grain*.14,1-smoothstep(.08,.4,length(fwidth(input.worldPosition.xz))));
	return shadeCity(input,float4(albedo,1));
}

float4 Asphalt_PS(s3d::PSInput input) : SV_TARGET
{
	float4 albedo = GetDiffuseColor(input.uv);
	float grey = dot(albedo.rgb,float3(.2126,.7152,.0722));
	albedo.rgb = lerp(albedo.rgb,grey*float3(.99,1.0,1.035),.88);
	float2 p = input.worldPosition.xz;
	float variation = sin(p.x*.18+sin(p.y*.27))*sin(p.y*.13+p.x*.037);
	albedo.rgb *= 1+variation*.085;
	float2 cell = floor(p/19);
	float2 within = frac(p/19);
	float patch = step(.83,fieldNoise(cell)) * step(.23,within.x)*step(within.x,.39)*step(.2,within.y)*step(within.y,.66);
	albedo.rgb *= 1-patch*.15;
	float3 detail = g_earthNormal.Sample(g_sampler0,input.uv).xyz*2-1;
	float strength = .10*(1-smoothstep(.15,1.0,length(fwidth(p))));
	input.normal = normalize(input.normal+float3(detail.x,0,detail.y)*strength);
	return shadeCity(input,albedo);
}


// Baked building atlases use blue-grey texels for glass. Approximate sky reflection
// with Fresnel; preserve the photographed recesses and diffuse facade shading.
float4 Building_PS(s3d::PSInput input) : SV_TARGET
{
	float4 albedo = GetDiffuseColor(input.uv);
	float glass = g_hasTexture * smoothstep(.008,.035,albedo.b-albedo.r) * (1-smoothstep(.10,.25,albedo.r)) * (1-smoothstep(.1,.4,abs(input.normal.y)));
	float3 view = normalize(g_eyePosition-input.worldPosition);
	float3 reflected = reflect(-view,normalize(input.normal));
	float fresnel = .04+.38*pow(1-saturate(abs(dot(view,normalize(input.normal)))),5);
	float3 sky = lerp(float3(.13,.16,.18),float3(.34,.47,.59),smoothstep(-.18,.6,reflected.y));
	float shade = .88+.12*sin(reflected.x*23+reflected.z*11);
	albedo.rgb = lerp(albedo.rgb,sky*shade,glass*fresnel);
	return shadeCity(input,albedo);
}

// Color is instance data, so thousands of differently painted cars share a draw.
struct VehiclePSInput
{
    float4 position : SV_POSITION; float3 worldPosition : TEXCOORD0;
    float2 uv : TEXCOORD1; float3 normal : TEXCOORD2; float4 paint : TEXCOORD3;
};
float4 VehicleInstance_PS(VehiclePSInput input) : SV_TARGET
{
    s3d::PSInput surface;
    surface.position=input.position;surface.worldPosition=input.worldPosition;
    surface.uv=input.uv;surface.normal=input.normal;
    return shadeCity(surface,GetDiffuseColor(input.uv)*input.paint);
}
cbuffer VehiclePaint : register(b5) { float4 g_vehiclePaint; }
float4 VehiclePaint_PS(s3d::PSInput input) : SV_TARGET
{
    float4 albedo=GetDiffuseColor(input.uv);
    // Source coachwork is neutral .78/.80/.79. Glass, rubber, lamps and alloy remain unchanged.
    float paintMask=smoothstep(.64,.76,min(albedo.r,min(albedo.g,albedo.b)));
    paintMask*=1-smoothstep(.04,.12,max(albedo.r,max(albedo.g,albedo.b))-min(albedo.r,min(albedo.g,albedo.b)));
    albedo.rgb*=lerp(float3(1,1,1),g_vehiclePaint.rgb/float3(.78,.80,.79),paintMask);
    return shadeCity(input,albedo);
}

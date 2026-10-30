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

float hash21(float2 p)
{
	return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}
float valueNoise(float2 p)
{
	float2 i = floor(p), f = frac(p);
	f = f * f * (3 - 2 * f);
	return lerp(lerp(hash21(i), hash21(i + float2(1, 0)), f.x), lerp(hash21(i + float2(0, 1)), hash21(i + 1), f.x), f.y);
}
// Unmanaged Japanese land is not lawn: summer weeds, dry grass, bare earth and dark scrub form a mottled cover.
// Town lots and gardens shift the same mosaic drier or greener, so 16 m material cells do not show as tiles.
// Fine octaves fade out where a texel covers them, so distant land stays calm instead of shimmering.
float3 groundCover(float2 p, float cover, float steepness, float footprint)
{
	float dryness = cover > .625 ? 0 : (cover > .375 ? .08 : -.12);
	float fineWeight = 1 - smoothstep(1.5, 6, footprint);
	float large = valueNoise(p / 140), mid = valueNoise(p / 41 + 17.3);
	float fine = lerp(.5, valueNoise(p / 11 + 41.7) * .7 + valueNoise(p / 3.7 + 7.1) * .3, fineWeight);
	float patches = valueNoise(p / 23 + 91.3);
	float3 weeds = float3(.062, .100, .024), olive = float3(.092, .104, .038);
	float3 straw = float3(.150, .130, .060), scrub = float3(.038, .060, .020), earth = float3(.140, .112, .072);
	float3 result = lerp(weeds, olive, smoothstep(.25, .75, large * .5 + mid * .3 + fine * .2 + dryness));
	result = lerp(result, straw, smoothstep(.62, .86, mid * .6 + fine * .4 + dryness) * .6);
	result = lerp(result, scrub, smoothstep(.36, .16, mid * .65 + fine * .35 + dryness) * .5);
	result = lerp(result, earth, smoothstep(.60, .85, patches) * saturate(dryness * 8) * .5);
	result = lerp(result, scrub, saturate(steepness * 4) * .5);
	return lerp(result, float3(.060, .102, .026), saturate(-dryness * 4) * .45);
}

// Town ground: UV carries the paved and tended-green share interpolated between 16 m cells.
// A noisy threshold turns the smooth shares into irregular paved yards and weedy patches instead of a cell staircase.
float3 townGround(float2 ground, float2 shares, float steepness, float footprint, float grain)
{
	float edge = valueNoise(ground / 5.3 + 7.7) * .6 + valueNoise(ground / 17 + 2.1) * .4 - .5;
	float paved = smoothstep(.46, .62, shares.x + edge * .55);
	float tended = smoothstep(.30, .50, shares.y + edge * .45);
	float3 green = lerp(groundCover(ground, .5, steepness, footprint), groundCover(ground, .25, steepness, footprint), tended);
	green *= lerp(1, clamp(grain, .55, 1.5), .7);
	float slab = valueNoise(ground / 1.8 + 3.3) * .5 + valueNoise(ground / 7 + 9.1) * .5;
	float3 paving = float3(.150, .150, .146) * lerp(.88, 1.10, slab);
	return lerp(green, paving, paved);
}

float4 Terrain_PS(s3d::PSInput input) : SV_TARGET
{
	float cover = g_diffuseColor.a;
	float2 ground = input.worldPosition.xz;
	float footprint = length(fwidth(ground));
	float steepness = 1 - saturate(normalize(input.normal).y);
	float2 shares = input.uv;
	float forest = cover > .625 && cover < .875 ? saturate(input.uv.x) : 0;
	if (cover < .125 || (cover > .625 && cover < .875))
	{
		// Same rotated 8 m tiling as TerrainSurfaceGeometry::terrainUvAt.
		float2 tile = ground / 8;
		input.uv = float2(.97237 * tile.x - .23345 * tile.y, .23345 * tile.x + .97237 * tile.y);
	}
	float4 albedo = GetDiffuseColor(input.uv);
	albedo.a = 1;
	if (g_hasTexture)
	{
		float2 rotatedUv = float2(-input.uv.y, input.uv.x) * 0.173 + float2(0.31, 0.67);
		float3 broad = g_texture0.Sample(g_sampler0, rotatedUv).rgb;
		// Mixing two scales breaks the repeating grass tile in aerial views.
		albedo.rgb = lerp(albedo.rgb, g_diffuseColor.rgb * broad, .42);
	}
	if (cover < .875)
	{
		// The photograph supplies blade-scale relief only; colour comes from the cover mosaic.
		float detail = dot(albedo.rgb / max(g_diffuseColor.rgb, .01), float3(.3, .6, .1)) / .17;
		albedo.rgb = cover < .125 ? townGround(ground, shares, steepness, footprint, detail)
			: groundCover(ground, cover, steepness, footprint) * lerp(1, clamp(detail, .55, 1.5), .7);
		// Forest floor and, from afar, the closed canopy: all LODs share the same cover, so forests
		// stay in place when individual trees stop being drawn.
		float crowns = valueNoise(ground / 9 + 1.3) * lerp(.5, 1, 1 - smoothstep(4, 16, footprint)) * .6 + valueNoise(ground / 41) * .4;
		// Matches the foliage palette of the drawn trees (cedar darker, broadleaf lighter) so the switch is not visible.
		float3 canopy = lerp(float3(.040, .072, .024), float3(.066, .112, .034), smoothstep(.4, .6, valueNoise(ground / 260 + 3.7))) * (.7 + crowns * .5);
		albedo.rgb = lerp(albedo.rgb, canopy, smoothstep(.15, .85, forest));
	}
	float3 sand = g_coastalSand.Sample(g_sampler0, input.uv).rgb * float3(0.76, 0.70, 0.55);
	float dryLand = smoothstep(0.25, 3.0, input.worldPosition.y);
	albedo.rgb = lerp(sand, albedo.rgb, dryLand);
	float variation = sin(ground.x * .008 + sin(ground.y * .006)) * sin(ground.y * .009);
	float altitude = input.worldPosition.y + variation * g_terrainVariation.x;
	float alpine = smoothstep(g_altitudeBands.x, g_altitudeBands.y, altitude);
	float mineral = dot(albedo.rgb, float3(.2126,.7152,.0722));
	float3 alpineCover = lerp(float3(.25,.29,.16),float3(.35,.34,.32),saturate(steepness * 2 + alpine * .5));
	albedo.rgb = lerp(albedo.rgb,alpineCover * (.8 + mineral),alpine);
	float snow = smoothstep(g_altitudeBands.z, g_altitudeBands.w, altitude);
	float3 snowColor = float3(.86,.9,.95) * (1 + variation * .035);
	albedo.rgb = lerp(albedo.rgb,snowColor,snow * (1 - smoothstep(.55,.85,steepness)));
	return shadeCity(input, albedo);
}

// Japanese house lots seen from above: concrete and gravel parking, clipped shrubs and a little lawn.
// UV is metres along the lot axes from its centre, so paving slabs and planting beds line up with the house.
// Alpha selects a paved lot (1) or a garden lot (.5).
float4 Yard_PS(s3d::PSInput input) : SV_TARGET
{
	float2 p = input.worldPosition.xz;
	float2 uv = input.uv;
	float footprint = length(fwidth(uv));
	float garden = 1 - g_diffuseColor.a;
	float resolved = 1 - smoothstep(.5, 2.0, footprint);
	float2 bed = uv / float2(3.1, 2.3);
	float2 within = frac(bed);
	float cell = hash21(floor(bed) + floor(p / 40) * 7.3);
	// Bed edges blur slightly so the grid reads as gardens, not tiles.
	float edge = smoothstep(0, .12, min(min(within.x, 1 - within.x), min(within.y, 1 - within.y)));
	float n = cell * lerp(.6, 1, edge) * .8 + valueNoise(p / 6.5 + 13.7) * .2 - garden * .4;
	float3 gravel = g_texture0.Sample(g_sampler0, p / 3).rgb;
	float grain = dot(gravel, float3(.3, .6, .1)) / .22;
	float3 paved = float3(.120, .118, .112) * lerp(1, clamp(grain, .7, 1.3), .6);
	float3 lawn = float3(.064, .098, .030), shrub = float3(.026, .046, .018), soil = float3(.110, .092, .066);
	// Mostly lawn with a few clipped shrubs; the lawn itself varies only slightly.
	lawn *= .92 + valueNoise(p / 4.3 + 2.9) * .16;
	float3 plants = lerp(lawn, shrub, smoothstep(.62, .82, valueNoise(p / 1.6 + 5.1)));
	// Unresolved beds blend to the lot's average paving share instead of thresholding into large blotches.
	float share = lerp(lerp(.62, .28, garden), smoothstep(.36, .5, n), resolved);
	float3 albedo = lerp(plants, paved, share);
	albedo = lerp(albedo, soil, smoothstep(.72, .9, valueNoise(p / 4.1 + 29.3)) * .4);
	return shadeCity(input, float4(albedo, 1));
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
	// UV.x carries the parcel number in 1024 m spans, so neighbouring parcels differ in crop and growth.
	float2 uv=input.uv;
	float parcel=floor(uv.x/1024);
	uv.x-=parcel*1024+512;
	float crop1=fieldNoise(float2(parcel,paddy ? 3.1 : 7.7)),crop2=fieldNoise(float2(parcel*1.37+.5,11.3));
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
	soil=lerp(float3(.085,.055,.032),float3(.17,.115,.066),saturate(dot(soil,float3(.3,.5,.2))*2));
	soil*=lerp(1, .75+.32*(ridge*.5+.5),resolved);
	float3 green=lerp(float3(.050,.095,.020),float3(.12,.17,.035),variation);
	float3 albedo;
	if (paddy)
	{
		float along=cos(uv.y/.30*6.2831853);
		float rice=crop*smoothstep(-.25,.65,along);
		float3 water=float3(.045,.068,.052)+sin(uv.y*2.4+sin(uv.x*.7))*.004;
		float fresnel=pow(1-saturate(dot(normalize(g_eyePosition-input.worldPosition),normalize(input.normal))),4);
		water=lerp(water,float3(.23,.29,.31),fresnel*.65);
		// 夏の水田は稲が水面をほぼ覆う。株の育ちと品種で区画ごとに緑の濃さ・黄みが変わる。
		float3 young=float3(.074,.150,.020),grown=float3(.122,.205,.028),ripe=float3(.165,.190,.040);
		float3 riceGreen=lerp(young,grown,crop1);
		riceGreen=lerp(riceGreen,ripe,smoothstep(.75,.95,crop2)*.8);
		riceGreen*=lerp(.88,1.12,variation);
		// 田植え直後の区画は水面が多く見える。
		float cover=lerp(.82,.55,step(.9,crop2));
		albedo=lerp(water,riceGreen,lerp(cover,saturate(rice*.6+.45),resolved));
	}
	else
	{
		// Intermittent planting leaves exposed soil between cultivated rows; some parcels are fallow or freshly tilled.
		float grown=lerp(.3,.95,crop1);
		// Grown crops spread across the furrows, young ones leave most of the ridge bare.
		crop=smoothstep(.9-grown*1.3,1.2-grown*1.3,ridge)*smoothstep(-.6,.2,cos(uv.y*7.4)+grown);
		green=lerp(green,float3(.035,.070,.020),step(.6,crop2)*.6);
		albedo=lerp(soil,green,lerp(grown,crop*lerp(.5,1,grown),resolved));
	}
	// Keep broad crop variation visible when the individual rows become subpixel.
	float2 broadUv=input.worldPosition.xz*.075;
	float broad=sin(broadUv.x+sin(broadUv.y*.63))*cos(broadUv.y+sin(broadUv.x*.42));
	albedo*=.93+.09*broad;
	// Growth is never uniform within a parcel: water depth, fertiliser and lodging leave lighter and darker patches,
	// and the planting rows read as faint streaks from afar.
	float2 world=input.worldPosition.xz;
	float patchy=valueNoise(world/11+parcel*3.7)*.6+valueNoise(world/4.3+parcel*1.9)*.4;
	albedo*=.86+.26*patchy;
	float streak=1-smoothstep(.2,1.2,footprint);
	albedo*=1+(1-streak)*.025*sin(uv.x*(paddy ? 1.9 : .9));
	float grey=dot(albedo,float3(.2126,.7152,.0722));
	albedo=lerp(grey.xxx,albedo,.85);
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


// Streamed riverbeds carry signed distance from the channel in UV.x and a
// local gravel extent in UV.y. The underlying terrain remains the walking bed.
float4 RiverBank_PS(s3d::PSInput input) : SV_TARGET
{
	float2 ground = input.worldPosition.xz;
	float bankDistance = input.uv.x, width = max(1, input.uv.y);
	float variation = sin(ground.x * .071 + sin(ground.y * .037)) * sin(ground.y * .061 + ground.x * .013);
	float coverage = 1 - smoothstep(width - 5, width + 2, bankDistance + variation * 2.3);
	if (coverage < .008) discard;
	float2 uv = float2(ground.x * .97237 - ground.y * .23345, ground.x * .23345 + ground.y * .97237) / 10;
	float2 rotated = float2(-uv.y,uv.x) * .173 + float2(.31,.67);
	float3 gravel = lerp(g_texture0.Sample(g_sampler0,uv).rgb,g_texture0.Sample(g_sampler0,rotated).rgb,.38);
	float dry = smoothstep(2,14,bankDistance);
	float3 soil = lerp(float3(.40,.37,.31),float3(.72,.69,.60),dry);
	return shadeCity(input,float4(gravel * soil * (1 + variation * .065),coverage));
}

float4 River_PS(s3d::PSInput input) : SV_TARGET
{
    float2 p = input.worldPosition.xz;
    float2 phase = float2(dot(p, float2(.073, .119)), dot(p, float2(-.107, .061)));
    float footprint = max(length(ddx(phase)), length(ddy(phase)));
    float detail = 1 - smoothstep(.25, .9, footprint);
    float ripple = sin(phase.x + sin(phase.y * .37)) * sin(phase.y + cos(phase.x * .41));
    input.normal = normalize(input.normal);
    float fresnel = pow(1 - saturate(dot(normalize(g_eyePosition - input.worldPosition), input.normal)), 4);
    float3 water = lerp(float3(.028,.115,.13), float3(.28,.42,.49), fresnel * .75);
    water = lerp(water, float3(.48,.57,.56), smoothstep(.08,.45,1-saturate(input.normal.y))*.65);
    float streamTexture = ripple * .008 + sin(phase.x * 2.7 + sin(phase.y * 1.3) * 1.5) * .004;
    water += streamTexture * detail;
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
cbuffer BuildingTint : register(b6) { float4 g_buildingTint; }
// Japanese roofs mix dark blue, charcoal and silver tiles with brown, rust and green sheet metal.
// Only dark pitched roof texels are recoloured; flat roofs and lots keep their colour and facades get a slight tone shift.
float3 buildingVariant(float3 albedo, float3 normal, float variant)
{
	if (variant < .5) { return albedo; }
	static const float3 roofs[8] = {
		float3(1, 1, 1), float3(1.05, 1.08, 1.0), float3(1.9, 1.35, .95), float3(2.2, 1.05, .75),
		float3(.95, 1.25, 1.0), float3(2.4, 2.4, 2.3), float3(1, 1, 1), float3(1.5, 1.5, 1.45) };
	float lum = dot(albedo, float3(.2126, .7152, .0722));
	int index = (int)variant - 1;
	float roof = smoothstep(.5, .7, normal.y) * (1 - smoothstep(.95, .99, normal.y)) * (1 - smoothstep(.06, .14, lum));
	float3 recoloured = lum * roofs[index];
	albedo = lerp(albedo, index == 0 || index == 6 ? albedo : recoloured, roof);
	float wall = 1 - smoothstep(.3, .5, abs(normal.y));
	float3 tone = lerp(float3(1.04, 1.0, .95), float3(.95, .98, 1.03), frac(variant * .37)) * lerp(.92, 1.08, frac(variant * .61));
	return lerp(albedo, albedo * tone, wall);
}

float4 Building_PS(s3d::PSInput input) : SV_TARGET
{
	// Merged distant meshes carry the variant in the integer part of U; instances pass it in a constant buffer.
	float baked = floor(input.uv.x);
	input.uv.x -= baked;
	float variant = baked > .5 ? baked : g_buildingTint.x;
	float4 albedo = GetDiffuseColor(input.uv);
	albedo.rgb = buildingVariant(albedo.rgb, normalize(input.normal), variant);
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

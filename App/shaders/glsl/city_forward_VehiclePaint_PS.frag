// Generated from the matching HLSL entry point by chore/generate_linux_shaders.py.
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
#version 410
#if defined(GL_EXT_control_flow_attributes)
#extension GL_EXT_control_flow_attributes : require
#define SPIRV_CROSS_FLATTEN [[flatten]]
#define SPIRV_CROSS_BRANCH [[dont_flatten]]
#define SPIRV_CROSS_UNROLL [[unroll]]
#define SPIRV_CROSS_LOOP [[dont_unroll]]
#else
#define SPIRV_CROSS_FLATTEN
#define SPIRV_CROSS_BRANCH
#define SPIRV_CROSS_UNROLL
#define SPIRV_CROSS_LOOP
#endif

vec4 _982;

layout(std140) uniform PSPerMaterial
{
    vec3 g_ambientColor;
    uint g_hasTexture;
    vec4 g_diffuseColor;
    vec3 g_specularColor;
    float g_shininess;
    vec3 g_emissionColor;
} _55;

layout(std140) uniform CityParameters
{
    mat4 g_worldToShadow;
    vec4 g_shadowParameters;
    vec4 g_fogColorDensity;
    vec4 g_dynamicShadow;
    vec4 g_altitudeBands;
    vec4 g_terrainVariation;
} _124;

layout(std140) uniform PSPerFrame
{
    vec3 g_globalAmbientColor;
    vec3 g_sunColor;
    vec3 g_sunDirection;
} _191;

layout(std140) uniform PSPerView
{
    vec3 g_eyePosition;
} _358;

layout(std140) uniform VehiclePaint
{
    vec4 g_vehiclePaint;
} _472;

uniform sampler2D Texture0;
uniform sampler2D Texture1;
uniform sampler2D Texture3;

layout(location = 0) in vec3 input_worldPosition;
layout(location = 1) in vec2 input_uv;
layout(location = 2) in vec3 input_normal;
layout(location = 0) out vec4 _entryPointOutput;

void main()
{
    vec4 _971;
    if (_55.g_hasTexture != 0u)
    {
        _971 = _55.g_diffuseColor * texture(Texture0, input_uv);
    }
    else
    {
        _971 = _55.g_diffuseColor;
    }
    vec3 _589 = _971.xyz * mix(vec3(1.0), _472.g_vehiclePaint.xyz * vec3(1.2820513248443603515625, 1.25, 1.26582276821136474609375), vec3(smoothstep(0.63999998569488525390625, 0.7599999904632568359375, min(_971.x, min(_971.y, _971.z))) * (1.0 - smoothstep(0.039999999105930328369140625, 0.119999997317790985107421875, max(_971.x, max(_971.y, _971.z)) - min(_971.x, min(_971.y, _971.z))))));
    vec4 _949;
    _949.x = _589.x;
    _949.y = _589.y;
    _949.z = _589.z;
    vec3 _644 = normalize(input_normal);
    float _980;
    do
    {
        if (_124.g_shadowParameters.w <= 0.0)
        {
            _980 = 1.0;
            break;
        }
        vec4 _751 = _124.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _757 = _751.xyz / vec3(_751.w);
        vec2 _762 = (_757.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_762, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_762, vec2(0.99800002574920654296875)))) || (_757.z < 0.0)) || (_757.z > 1.0))
        {
            _980 = 1.0;
            break;
        }
        float _790 = _124.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_644, _191.g_sunDirection), 0.0, 1.0))));
        float _973;
        _973 = 0.0;
        float _978;
        SPIRV_CROSS_UNROLL
        for (int _972 = -1; _972 <= 1; _973 = _978, _972++)
        {
            _978 = _973;
            float _849;
            SPIRV_CROSS_UNROLL
            for (int _974 = -1; _974 <= 1; _978 = _849, _974++)
            {
                vec4 _815 = textureLod(Texture1, _762 + (vec2(float(_974), float(_972)) * _124.g_shadowParameters.xy), 0.0);
                float _816 = _815.x;
                float _976;
                if (_124.g_dynamicShadow.x > 0.5)
                {
                    _976 = max(_816, textureLod(Texture3, _762 + (vec2(float(_974), float(_972)) * _124.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _976 = _816;
                }
                _849 = _978 + float(int((_757.z + _790) >= _976));
            }
        }
        _980 = mix(1.0, _973 * 0.111111111938953399658203125, _124.g_shadowParameters.w * clamp(min(min(_762.x, _762.y), min(1.0 - _762.x, 1.0 - _762.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _652 = _191.g_sunColor * _980;
    vec3 _718 = mix(_124.g_fogColorDensity.xyz, ((((_191.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_644.y * 0.5) + 0.5, 0.0, 1.0)))) + (_652 * clamp(dot(_644, _191.g_sunDirection), 0.0, 1.0))) * _949.xyz) + ((_652 * (_55.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_644, normalize(normalize(_358.g_eyePosition - input_worldPosition) + _191.g_sunDirection)), 0.0, 1.0), max(8.0, _55.g_shininess)) * float(0.0 < dot(_644, _191.g_sunDirection))))) + _55.g_emissionColor, vec3(exp((-_124.g_fogColorDensity.w) * distance(_358.g_eyePosition, input_worldPosition))));
    _entryPointOutput = vec4(_718, _971.w);
}


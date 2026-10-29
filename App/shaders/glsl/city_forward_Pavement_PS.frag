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

layout(std140) uniform CityParameters
{
    mat4 g_worldToShadow;
    vec4 g_shadowParameters;
    vec4 g_fogColorDensity;
    vec4 g_dynamicShadow;
    vec4 g_altitudeBands;
    vec4 g_terrainVariation;
} _89;

layout(std140) uniform PSPerFrame
{
    vec3 g_globalAmbientColor;
    vec3 g_sunColor;
    vec3 g_sunDirection;
} _159;

layout(std140) uniform PSPerView
{
    vec3 g_eyePosition;
} _334;

layout(std140) uniform PSPerMaterial
{
    vec3 g_ambientColor;
    uint g_hasTexture;
    vec4 g_diffuseColor;
    vec3 g_specularColor;
    float g_shininess;
    vec3 g_emissionColor;
} _351;

uniform sampler2D Texture1;
uniform sampler2D Texture3;

layout(location = 0) in vec3 input_worldPosition;
layout(location = 1) in vec2 input_uv;
layout(location = 2) in vec3 input_normal;
layout(location = 0) out vec4 _entryPointOutput;

void main()
{
    vec2 _597 = input_uv * vec2(2.5, 1.66666662693023681640625);
    _597.y = _597.y + (mod(floor(_597.x), 2.0) * 0.5);
    vec2 _615 = fwidth(_597);
    vec2 _616 = max(_615, vec2(9.9999997473787516355514526367188e-05));
    vec2 _623 = vec2(1.0) - smoothstep(vec2(0.008000000379979610443115234375), vec2(0.008000000379979610443115234375) + _616, min(fract(_597), vec2(1.0) - fract(_597)));
    vec2 _664 = fwidth(input_worldPosition.xz);
    vec3 _719 = normalize(input_normal);
    float _1046;
    do
    {
        if (_89.g_shadowParameters.w <= 0.0)
        {
            _1046 = 1.0;
            break;
        }
        vec4 _826 = _89.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _832 = _826.xyz / vec3(_826.w);
        vec2 _837 = (_832.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_837, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_837, vec2(0.99800002574920654296875)))) || (_832.z < 0.0)) || (_832.z > 1.0))
        {
            _1046 = 1.0;
            break;
        }
        float _865 = _89.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_719, _159.g_sunDirection), 0.0, 1.0))));
        float _1039;
        _1039 = 0.0;
        float _1044;
        SPIRV_CROSS_UNROLL
        for (int _1038 = -1; _1038 <= 1; _1039 = _1044, _1038++)
        {
            _1044 = _1039;
            float _924;
            SPIRV_CROSS_UNROLL
            for (int _1040 = -1; _1040 <= 1; _1044 = _924, _1040++)
            {
                vec4 _890 = textureLod(Texture1, _837 + (vec2(float(_1040), float(_1038)) * _89.g_shadowParameters.xy), 0.0);
                float _891 = _890.x;
                float _1042;
                if (_89.g_dynamicShadow.x > 0.5)
                {
                    _1042 = max(_891, textureLod(Texture3, _837 + (vec2(float(_1040), float(_1038)) * _89.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _1042 = _891;
                }
                _924 = _1044 + float(int((_832.z + _865) >= _1042));
            }
        }
        _1046 = mix(1.0, _1039 * 0.111111111938953399658203125, _89.g_shadowParameters.w * clamp(min(min(_837.x, _837.y), min(1.0 - _837.x, 1.0 - _837.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _727 = _159.g_sunColor * _1046;
    vec3 _776 = ((((_159.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_719.y * 0.5) + 0.5, 0.0, 1.0)))) + (_727 * clamp(dot(_719, _159.g_sunDirection), 0.0, 1.0))) * vec4(((_351.g_diffuseColor.xyz * mix(0.87999999523162841796875, 1.12000000476837158203125, fract(sin(dot(floor(_597), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875))) * (1.0 - ((max(_623.x, _623.y) * 0.36000001430511474609375) * (1.0 - smoothstep(0.20000000298023223876953125, 1.0, max(_616.x, _616.y)))))) * mix(1.0, 0.930000007152557373046875 + (fract(sin(dot(floor(input_worldPosition.xz * 160.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875) * 0.14000000059604644775390625), 1.0 - smoothstep(0.07999999821186065673828125, 0.4000000059604644775390625, length(_664))), 1.0).xyz) + ((_727 * (_351.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_719, normalize(normalize(_334.g_eyePosition - input_worldPosition) + _159.g_sunDirection)), 0.0, 1.0), max(8.0, _351.g_shininess)) * float(0.0 < dot(_719, _159.g_sunDirection))))) + _351.g_emissionColor;
    _entryPointOutput = vec4(mix(_89.g_fogColorDensity.xyz, _776, vec3(exp((-_89.g_fogColorDensity.w) * distance(_334.g_eyePosition, input_worldPosition)))), 1.0);
}


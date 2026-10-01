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
    vec2 _594 = input_uv * vec2(2.5, 1.66666662693023681640625);
    _594.y = _594.y + (mod(floor(_594.x), 2.0) * 0.5);
    vec2 _612 = fwidth(_594);
    vec2 _613 = max(_612, vec2(9.9999997473787516355514526367188e-05));
    vec2 _620 = vec2(1.0) - smoothstep(vec2(0.008000000379979610443115234375), vec2(0.008000000379979610443115234375) + _613, min(fract(_594), vec2(1.0) - fract(_594)));
    vec2 _661 = fwidth(input_worldPosition.xz);
    vec3 _716 = normalize(input_normal);
    float _1043;
    do
    {
        if (_89.g_shadowParameters.w <= 0.0)
        {
            _1043 = 1.0;
            break;
        }
        vec4 _823 = _89.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _829 = _823.xyz / vec3(_823.w);
        vec2 _834 = (_829.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_834, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_834, vec2(0.99800002574920654296875)))) || (_829.z < 0.0)) || (_829.z > 1.0))
        {
            _1043 = 1.0;
            break;
        }
        float _862 = _89.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_716, _159.g_sunDirection), 0.0, 1.0))));
        float _1036;
        _1036 = 0.0;
        float _1041;
        SPIRV_CROSS_UNROLL
        for (int _1035 = -1; _1035 <= 1; _1036 = _1041, _1035++)
        {
            _1041 = _1036;
            float _921;
            SPIRV_CROSS_UNROLL
            for (int _1037 = -1; _1037 <= 1; _1041 = _921, _1037++)
            {
                vec4 _887 = textureLod(Texture1, _834 + (vec2(float(_1037), float(_1035)) * _89.g_shadowParameters.xy), 0.0);
                float _888 = _887.x;
                float _1039;
                if (_89.g_dynamicShadow.x > 0.5)
                {
                    _1039 = max(_888, textureLod(Texture3, _834 + (vec2(float(_1037), float(_1035)) * _89.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _1039 = _888;
                }
                _921 = _1041 + float(int((_829.z + _862) >= _1039));
            }
        }
        _1043 = mix(1.0, _1036 * 0.111111111938953399658203125, _89.g_shadowParameters.w * clamp(min(min(_834.x, _834.y), min(1.0 - _834.x, 1.0 - _834.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _724 = _159.g_sunColor * _1043;
    vec3 _773 = ((((_159.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_716.y * 0.5) + 0.5, 0.0, 1.0)))) + (_724 * clamp(dot(_716, _159.g_sunDirection), 0.0, 1.0))) * vec4(((_351.g_diffuseColor.xyz * mix(0.87999999523162841796875, 1.12000000476837158203125, fract(sin(dot(floor(_594), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875))) * (1.0 - ((max(_620.x, _620.y) * 0.36000001430511474609375) * (1.0 - smoothstep(0.20000000298023223876953125, 1.0, max(_613.x, _613.y)))))) * mix(1.0, 0.930000007152557373046875 + (fract(sin(dot(floor(input_worldPosition.xz * 160.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875) * 0.14000000059604644775390625), 1.0 - smoothstep(0.07999999821186065673828125, 0.4000000059604644775390625, length(_661))), 1.0).xyz) + ((_724 * (_351.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_716, normalize(normalize(_334.g_eyePosition - input_worldPosition) + _159.g_sunDirection)), 0.0, 1.0), max(8.0, _351.g_shininess)) * float(0.0 < dot(_716, _159.g_sunDirection))))) + _351.g_emissionColor;
    _entryPointOutput = vec4(mix(_89.g_fogColorDensity.xyz, _773, vec3(exp((-_89.g_fogColorDensity.w) * distance(_334.g_eyePosition, input_worldPosition)))), 1.0);
}


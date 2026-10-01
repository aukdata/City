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
} _84;

layout(std140) uniform PSPerFrame
{
    vec3 g_globalAmbientColor;
    vec3 g_sunColor;
    vec3 g_sunDirection;
} _155;

layout(std140) uniform PSPerView
{
    vec3 g_eyePosition;
} _330;

layout(std140) uniform PSPerMaterial
{
    vec3 g_ambientColor;
    uint g_hasTexture;
    vec4 g_diffuseColor;
    vec3 g_specularColor;
    float g_shininess;
    vec3 g_emissionColor;
} _347;

uniform sampler2D Texture1;
uniform sampler2D Texture3;

layout(location = 0) in vec3 input_worldPosition;
layout(location = 2) in vec3 input_normal;
layout(location = 0) out vec4 _entryPointOutput;

void main()
{
    float _599 = dot(input_worldPosition.xz, vec2(0.07299999892711639404296875, 0.11900000274181365966796875));
    float _601 = dot(input_worldPosition.xz, vec2(-0.107000000774860382080078125, 0.0610000006854534149169921875));
    vec2 _602 = vec2(_599, _601);
    vec2 _604 = dFdx(_602);
    vec2 _607 = dFdy(_602);
    vec3 _632 = normalize(input_normal);
    vec3 _713 = normalize(_632);
    float _1033;
    do
    {
        if (_84.g_shadowParameters.w <= 0.0)
        {
            _1033 = 1.0;
            break;
        }
        vec4 _820 = _84.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _826 = _820.xyz / vec3(_820.w);
        vec2 _831 = (_826.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_831, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_831, vec2(0.99800002574920654296875)))) || (_826.z < 0.0)) || (_826.z > 1.0))
        {
            _1033 = 1.0;
            break;
        }
        float _859 = _84.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_713, _155.g_sunDirection), 0.0, 1.0))));
        float _1026;
        _1026 = 0.0;
        float _1031;
        SPIRV_CROSS_UNROLL
        for (int _1025 = -1; _1025 <= 1; _1026 = _1031, _1025++)
        {
            _1031 = _1026;
            float _918;
            SPIRV_CROSS_UNROLL
            for (int _1027 = -1; _1027 <= 1; _1031 = _918, _1027++)
            {
                vec4 _884 = textureLod(Texture1, _831 + (vec2(float(_1027), float(_1025)) * _84.g_shadowParameters.xy), 0.0);
                float _885 = _884.x;
                float _1029;
                if (_84.g_dynamicShadow.x > 0.5)
                {
                    _1029 = max(_885, textureLod(Texture3, _831 + (vec2(float(_1027), float(_1025)) * _84.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _1029 = _885;
                }
                _918 = _1031 + float(int((_826.z + _859) >= _1029));
            }
        }
        _1033 = mix(1.0, _1026 * 0.111111111938953399658203125, _84.g_shadowParameters.w * clamp(min(min(_831.x, _831.y), min(1.0 - _831.x, 1.0 - _831.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _721 = _155.g_sunColor * _1033;
    vec3 _767 = (((_155.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_713.y * 0.5) + 0.5, 0.0, 1.0)))) + (_721 * clamp(dot(_713, _155.g_sunDirection), 0.0, 1.0))) * vec4(mix(mix(vec3(0.0280000008642673492431640625, 0.115000002086162567138671875, 0.12999999523162841796875), vec3(0.2800000011920928955078125, 0.4199999868869781494140625, 0.4900000095367431640625), vec3(pow(1.0 - clamp(dot(normalize(_330.g_eyePosition - input_worldPosition), _632), 0.0, 1.0), 4.0) * 0.75)), vec3(0.4799999892711639404296875, 0.569999992847442626953125, 0.560000002384185791015625), vec3(smoothstep(0.07999999821186065673828125, 0.449999988079071044921875, 1.0 - clamp(_632.y, 0.0, 1.0)) * 0.64999997615814208984375)) + vec3((((sin(_599 + sin(_601 * 0.37000000476837158203125)) * sin(_601 + cos(_599 * 0.4099999964237213134765625))) * 0.008000000379979610443115234375) + (sin((_599 * 2.7000000476837158203125) + (sin(_601 * 1.2999999523162841796875) * 1.5)) * 0.0040000001899898052215576171875)) * (1.0 - smoothstep(0.25, 0.89999997615814208984375, max(length(_604), length(_607))))), 1.0).xyz) + ((_721 * (_347.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_713, normalize(normalize(_330.g_eyePosition - input_worldPosition) + _155.g_sunDirection)), 0.0, 1.0), max(8.0, _347.g_shininess)) * float(0.0 < dot(_713, _155.g_sunDirection))));
    _entryPointOutput = vec4(mix(_84.g_fogColorDensity.xyz, _767 + _347.g_emissionColor, vec3(exp((-_84.g_fogColorDensity.w) * distance(_330.g_eyePosition, input_worldPosition)))), 1.0);
}


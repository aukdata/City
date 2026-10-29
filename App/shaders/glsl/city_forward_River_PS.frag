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
    float _602 = dot(input_worldPosition.xz, vec2(0.07299999892711639404296875, 0.11900000274181365966796875));
    float _604 = dot(input_worldPosition.xz, vec2(-0.107000000774860382080078125, 0.0610000006854534149169921875));
    vec2 _605 = vec2(_602, _604);
    vec2 _607 = dFdx(_605);
    vec2 _610 = dFdy(_605);
    vec3 _635 = normalize(input_normal);
    vec3 _716 = normalize(_635);
    float _1036;
    do
    {
        if (_84.g_shadowParameters.w <= 0.0)
        {
            _1036 = 1.0;
            break;
        }
        vec4 _823 = _84.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _829 = _823.xyz / vec3(_823.w);
        vec2 _834 = (_829.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_834, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_834, vec2(0.99800002574920654296875)))) || (_829.z < 0.0)) || (_829.z > 1.0))
        {
            _1036 = 1.0;
            break;
        }
        float _862 = _84.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_716, _155.g_sunDirection), 0.0, 1.0))));
        float _1029;
        _1029 = 0.0;
        float _1034;
        SPIRV_CROSS_UNROLL
        for (int _1028 = -1; _1028 <= 1; _1029 = _1034, _1028++)
        {
            _1034 = _1029;
            float _921;
            SPIRV_CROSS_UNROLL
            for (int _1030 = -1; _1030 <= 1; _1034 = _921, _1030++)
            {
                vec4 _887 = textureLod(Texture1, _834 + (vec2(float(_1030), float(_1028)) * _84.g_shadowParameters.xy), 0.0);
                float _888 = _887.x;
                float _1032;
                if (_84.g_dynamicShadow.x > 0.5)
                {
                    _1032 = max(_888, textureLod(Texture3, _834 + (vec2(float(_1030), float(_1028)) * _84.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _1032 = _888;
                }
                _921 = _1034 + float(int((_829.z + _862) >= _1032));
            }
        }
        _1036 = mix(1.0, _1029 * 0.111111111938953399658203125, _84.g_shadowParameters.w * clamp(min(min(_834.x, _834.y), min(1.0 - _834.x, 1.0 - _834.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _724 = _155.g_sunColor * _1036;
    vec3 _770 = (((_155.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_716.y * 0.5) + 0.5, 0.0, 1.0)))) + (_724 * clamp(dot(_716, _155.g_sunDirection), 0.0, 1.0))) * vec4(mix(mix(vec3(0.0280000008642673492431640625, 0.115000002086162567138671875, 0.12999999523162841796875), vec3(0.2800000011920928955078125, 0.4199999868869781494140625, 0.4900000095367431640625), vec3(pow(1.0 - clamp(dot(normalize(_330.g_eyePosition - input_worldPosition), _635), 0.0, 1.0), 4.0) * 0.75)), vec3(0.4799999892711639404296875, 0.569999992847442626953125, 0.560000002384185791015625), vec3(smoothstep(0.07999999821186065673828125, 0.449999988079071044921875, 1.0 - clamp(_635.y, 0.0, 1.0)) * 0.64999997615814208984375)) + vec3((((sin(_602 + sin(_604 * 0.37000000476837158203125)) * sin(_604 + cos(_602 * 0.4099999964237213134765625))) * 0.008000000379979610443115234375) + (sin((_602 * 2.7000000476837158203125) + (sin(_604 * 1.2999999523162841796875) * 1.5)) * 0.0040000001899898052215576171875)) * (1.0 - smoothstep(0.25, 0.89999997615814208984375, max(length(_607), length(_610))))), 1.0).xyz) + ((_724 * (_347.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_716, normalize(normalize(_330.g_eyePosition - input_worldPosition) + _155.g_sunDirection)), 0.0, 1.0), max(8.0, _347.g_shininess)) * float(0.0 < dot(_716, _155.g_sunDirection))));
    _entryPointOutput = vec4(mix(_84.g_fogColorDensity.xyz, _770 + _347.g_emissionColor, vec3(exp((-_84.g_fogColorDensity.w) * distance(_330.g_eyePosition, input_worldPosition)))), 1.0);
}


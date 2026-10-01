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
layout(location = 2) in vec3 input_normal;
layout(location = 0) out vec4 _entryPointOutput;

void main()
{
    vec3 _570 = input_worldPosition * 18.0;
    vec3 _572 = fwidth(_570);
    float _575 = 1.0 - smoothstep(0.60000002384185791015625, 2.0, length(_572));
    vec3 _671 = normalize(normalize(input_normal + ((vec3(sin(_570.x), 0.0, cos(_570.z)) * 0.180000007152557373046875) * _575)));
    float _992;
    do
    {
        if (_89.g_shadowParameters.w <= 0.0)
        {
            _992 = 1.0;
            break;
        }
        vec4 _778 = _89.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _784 = _778.xyz / vec3(_778.w);
        vec2 _789 = (_784.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_789, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_789, vec2(0.99800002574920654296875)))) || (_784.z < 0.0)) || (_784.z > 1.0))
        {
            _992 = 1.0;
            break;
        }
        float _817 = _89.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_671, _159.g_sunDirection), 0.0, 1.0))));
        float _985;
        _985 = 0.0;
        float _990;
        SPIRV_CROSS_UNROLL
        for (int _984 = -1; _984 <= 1; _985 = _990, _984++)
        {
            _990 = _985;
            float _876;
            SPIRV_CROSS_UNROLL
            for (int _986 = -1; _986 <= 1; _990 = _876, _986++)
            {
                vec4 _842 = textureLod(Texture1, _789 + (vec2(float(_986), float(_984)) * _89.g_shadowParameters.xy), 0.0);
                float _843 = _842.x;
                float _988;
                if (_89.g_dynamicShadow.x > 0.5)
                {
                    _988 = max(_843, textureLod(Texture3, _789 + (vec2(float(_986), float(_984)) * _89.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _988 = _843;
                }
                _876 = _990 + float(int((_784.z + _817) >= _988));
            }
        }
        _992 = mix(1.0, _985 * 0.111111111938953399658203125, _89.g_shadowParameters.w * clamp(min(min(_789.x, _789.y), min(1.0 - _789.x, 1.0 - _789.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _679 = _159.g_sunColor * _992;
    vec3 _728 = ((((_159.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_671.y * 0.5) + 0.5, 0.0, 1.0)))) + (_679 * clamp(dot(_671, _159.g_sunDirection), 0.0, 1.0))) * vec4((_351.g_diffuseColor.xyz * mix(0.800000011920928955078125, 1.10000002384185791015625, clamp(0.449999988079071044921875 + ((sin((input_worldPosition.x * 0.730000019073486328125) + sin(input_worldPosition.z * 0.910000026226043701171875)) * cos(input_worldPosition.y * 1.37000000476837158203125)) * 0.300000011920928955078125), 0.0, 1.0))) * mix(1.0, 0.579999983310699462890625 + (fract(sin(dot(floor(_570.xz + vec2(_570.y * 0.430000007152557373046875)), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875) * 0.839999973773956298828125), _575), 1.0).xyz) + ((_679 * (_351.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_671, normalize(normalize(_334.g_eyePosition - input_worldPosition) + _159.g_sunDirection)), 0.0, 1.0), max(8.0, _351.g_shininess)) * float(0.0 < dot(_671, _159.g_sunDirection))))) + _351.g_emissionColor;
    _entryPointOutput = vec4(mix(_89.g_fogColorDensity.xyz, _728, vec3(exp((-_89.g_fogColorDensity.w) * distance(_334.g_eyePosition, input_worldPosition)))), 1.0);
}


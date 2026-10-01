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

vec4 _1057;

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

uniform sampler2D Texture0;
uniform sampler2D Texture1;
uniform sampler2D Texture3;

layout(location = 0) in vec3 input_worldPosition;
layout(location = 1) in vec2 input_uv;
layout(location = 2) in vec3 input_normal;
layout(location = 0) out vec4 _entryPointOutput;

void main()
{
    vec4 _1046;
    if (_55.g_hasTexture != 0u)
    {
        _1046 = _55.g_diffuseColor * texture(Texture0, input_uv);
    }
    else
    {
        _1046 = _55.g_diffuseColor;
    }
    vec3 _627 = normalize(_358.g_eyePosition - input_worldPosition);
    vec3 _633 = reflect(-_627, normalize(input_normal));
    vec3 _669 = mix(_1046.xyz, mix(vec3(0.12999999523162841796875, 0.1599999964237213134765625, 0.180000007152557373046875), vec3(0.3400000035762786865234375, 0.4699999988079071044921875, 0.589999973773956298828125), vec3(smoothstep(-0.180000007152557373046875, 0.60000002384185791015625, _633.y))) * (0.87999999523162841796875 + (0.119999997317790985107421875 * sin((_633.x * 23.0) + (_633.z * 11.0)))), vec3((((float(_55.g_hasTexture) * smoothstep(0.008000000379979610443115234375, 0.0350000001490116119384765625, _1046.z - _1046.x)) * (1.0 - smoothstep(0.100000001490116119384765625, 0.25, _1046.x))) * (1.0 - smoothstep(0.100000001490116119384765625, 0.4000000059604644775390625, abs(input_normal.y)))) * (0.039999999105930328369140625 + (0.37999999523162841796875 * pow(1.0 - clamp(abs(dot(_627, normalize(input_normal))), 0.0, 1.0), 5.0)))));
    vec4 _1028;
    _1028.x = _669.x;
    _1028.y = _669.y;
    _1028.z = _669.z;
    vec3 _724 = normalize(input_normal);
    float _1055;
    do
    {
        if (_124.g_shadowParameters.w <= 0.0)
        {
            _1055 = 1.0;
            break;
        }
        vec4 _831 = _124.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _837 = _831.xyz / vec3(_831.w);
        vec2 _842 = (_837.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_842, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_842, vec2(0.99800002574920654296875)))) || (_837.z < 0.0)) || (_837.z > 1.0))
        {
            _1055 = 1.0;
            break;
        }
        float _870 = _124.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_724, _191.g_sunDirection), 0.0, 1.0))));
        float _1048;
        _1048 = 0.0;
        float _1053;
        SPIRV_CROSS_UNROLL
        for (int _1047 = -1; _1047 <= 1; _1048 = _1053, _1047++)
        {
            _1053 = _1048;
            float _929;
            SPIRV_CROSS_UNROLL
            for (int _1049 = -1; _1049 <= 1; _1053 = _929, _1049++)
            {
                vec4 _895 = textureLod(Texture1, _842 + (vec2(float(_1049), float(_1047)) * _124.g_shadowParameters.xy), 0.0);
                float _896 = _895.x;
                float _1051;
                if (_124.g_dynamicShadow.x > 0.5)
                {
                    _1051 = max(_896, textureLod(Texture3, _842 + (vec2(float(_1049), float(_1047)) * _124.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _1051 = _896;
                }
                _929 = _1053 + float(int((_837.z + _870) >= _1051));
            }
        }
        _1055 = mix(1.0, _1048 * 0.111111111938953399658203125, _124.g_shadowParameters.w * clamp(min(min(_842.x, _842.y), min(1.0 - _842.x, 1.0 - _842.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _732 = _191.g_sunColor * _1055;
    vec3 _798 = mix(_124.g_fogColorDensity.xyz, ((((_191.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_724.y * 0.5) + 0.5, 0.0, 1.0)))) + (_732 * clamp(dot(_724, _191.g_sunDirection), 0.0, 1.0))) * _1028.xyz) + ((_732 * (_55.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_724, normalize(normalize(_358.g_eyePosition - input_worldPosition) + _191.g_sunDirection)), 0.0, 1.0), max(8.0, _55.g_shininess)) * float(0.0 < dot(_724, _191.g_sunDirection))))) + _55.g_emissionColor, vec3(exp((-_124.g_fogColorDensity.w) * distance(_358.g_eyePosition, input_worldPosition))));
    _entryPointOutput = vec4(_798, _1046.w);
}


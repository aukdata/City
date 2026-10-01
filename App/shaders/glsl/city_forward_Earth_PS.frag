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
} _90;

layout(std140) uniform PSPerFrame
{
    vec3 g_globalAmbientColor;
    vec3 g_sunColor;
    vec3 g_sunDirection;
} _161;

layout(std140) uniform PSPerView
{
    vec3 g_eyePosition;
} _336;

layout(std140) uniform PSPerMaterial
{
    vec3 g_ambientColor;
    uint g_hasTexture;
    vec4 g_diffuseColor;
    vec3 g_specularColor;
    float g_shininess;
    vec3 g_emissionColor;
} _353;

uniform sampler2D Texture0;
uniform sampler2D Texture4;
uniform sampler2D Texture1;
uniform sampler2D Texture3;

layout(location = 0) in vec3 input_worldPosition;
layout(location = 1) in vec2 input_uv;
layout(location = 2) in vec3 input_normal;
layout(location = 0) out vec4 _entryPointOutput;

void main()
{
    vec4 _646 = texture(Texture0, input_uv);
    vec4 _652 = texture(Texture0, (vec2((input_uv.x * 0.800000011920928955078125) - (input_uv.y * 0.60000002384185791015625), (input_uv.x * 0.60000002384185791015625) + (input_uv.y * 0.800000011920928955078125)) * 1.31700003147125244140625) + vec2(0.370999991893768310546875, 0.828999996185302734375));
    vec4 _699 = texture(Texture4, input_uv);
    vec3 _703 = (_699.xyz * 2.0) - vec3(1.0);
    vec3 _706 = normalize(input_normal);
    vec4 _728 = vec4(((_646.xyz + _652.xyz) * 0.5) * (1.0 + ((sin((input_worldPosition.x * 0.189999997615814208984375) + sin(input_worldPosition.z * 0.14000000059604644775390625)) * sin((input_worldPosition.z * 0.23000000417232513427734375) + (input_worldPosition.x * 0.0900000035762786865234375))) * 0.070000000298023223876953125)), 1.0) * _353.g_diffuseColor;
    vec3 _758 = normalize(normalize(_706 + ((vec3(_703.x, 0.0, _703.y) * 0.2199999988079071044921875) * clamp(_706.y, 0.0, 1.0))));
    float _1096;
    do
    {
        if (_90.g_shadowParameters.w <= 0.0)
        {
            _1096 = 1.0;
            break;
        }
        vec4 _865 = _90.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _871 = _865.xyz / vec3(_865.w);
        vec2 _876 = (_871.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_876, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_876, vec2(0.99800002574920654296875)))) || (_871.z < 0.0)) || (_871.z > 1.0))
        {
            _1096 = 1.0;
            break;
        }
        float _904 = _90.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_758, _161.g_sunDirection), 0.0, 1.0))));
        float _1089;
        _1089 = 0.0;
        float _1094;
        SPIRV_CROSS_UNROLL
        for (int _1088 = -1; _1088 <= 1; _1089 = _1094, _1088++)
        {
            _1094 = _1089;
            float _963;
            SPIRV_CROSS_UNROLL
            for (int _1090 = -1; _1090 <= 1; _1094 = _963, _1090++)
            {
                vec4 _929 = textureLod(Texture1, _876 + (vec2(float(_1090), float(_1088)) * _90.g_shadowParameters.xy), 0.0);
                float _930 = _929.x;
                float _1092;
                if (_90.g_dynamicShadow.x > 0.5)
                {
                    _1092 = max(_930, textureLod(Texture3, _876 + (vec2(float(_1090), float(_1088)) * _90.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _1092 = _930;
                }
                _963 = _1094 + float(int((_871.z + _904) >= _1092));
            }
        }
        _1096 = mix(1.0, _1089 * 0.111111111938953399658203125, _90.g_shadowParameters.w * clamp(min(min(_876.x, _876.y), min(1.0 - _876.x, 1.0 - _876.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _766 = _161.g_sunColor * _1096;
    _entryPointOutput = vec4(mix(_90.g_fogColorDensity.xyz, ((((_161.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_758.y * 0.5) + 0.5, 0.0, 1.0)))) + (_766 * clamp(dot(_758, _161.g_sunDirection), 0.0, 1.0))) * _728.xyz) + ((_766 * (_353.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_758, normalize(normalize(_336.g_eyePosition - input_worldPosition) + _161.g_sunDirection)), 0.0, 1.0), max(8.0, _353.g_shininess)) * float(0.0 < dot(_758, _161.g_sunDirection))))) + _353.g_emissionColor, vec3(exp((-_90.g_fogColorDensity.w) * distance(_336.g_eyePosition, input_worldPosition)))), _728.w);
}


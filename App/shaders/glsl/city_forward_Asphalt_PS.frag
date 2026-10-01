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

vec4 _1185;

layout(std140) uniform PSPerMaterial
{
    vec3 g_ambientColor;
    uint g_hasTexture;
    vec4 g_diffuseColor;
    vec3 g_specularColor;
    float g_shininess;
    vec3 g_emissionColor;
} _59;

layout(std140) uniform CityParameters
{
    mat4 g_worldToShadow;
    vec4 g_shadowParameters;
    vec4 g_fogColorDensity;
    vec4 g_dynamicShadow;
    vec4 g_altitudeBands;
    vec4 g_terrainVariation;
} _128;

layout(std140) uniform PSPerFrame
{
    vec3 g_globalAmbientColor;
    vec3 g_sunColor;
    vec3 g_sunDirection;
} _195;

layout(std140) uniform PSPerView
{
    vec3 g_eyePosition;
} _362;

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
    vec4 _1173;
    if (_59.g_hasTexture != 0u)
    {
        _1173 = _59.g_diffuseColor * texture(Texture0, input_uv);
    }
    else
    {
        _1173 = _59.g_diffuseColor;
    }
    vec3 _670 = mix(_1173.xyz, vec3(0.9900000095367431640625, 1.0, 1.03499996662139892578125) * dot(_1173.xyz, vec3(0.2125999927520751953125, 0.715200006961822509765625, 0.072200000286102294921875)), vec3(0.87999999523162841796875));
    vec4 _1128;
    _1128.x = _670.x;
    _1128.y = _670.y;
    _1128.z = _670.z;
    vec3 _703 = _1128.xyz * (1.0 + ((sin((input_worldPosition.x * 0.180000007152557373046875) + sin(input_worldPosition.z * 0.2700000107288360595703125)) * sin((input_worldPosition.z * 0.12999999523162841796875) + (input_worldPosition.x * 0.037000000476837158203125))) * 0.085000000894069671630859375));
    vec4 _1138;
    _1138.x = _703.x;
    _1138.y = _703.y;
    _1138.z = _703.z;
    vec2 _717 = fract(input_worldPosition.xz * vec2(0.052631579339504241943359375));
    vec3 _742 = _1138.xyz * (1.0 - (((((step(0.829999983310699462890625, fract(sin(dot(floor(input_worldPosition.xz * vec2(0.052631579339504241943359375)), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875)) * step(0.23000000417232513427734375, _717.x)) * step(_717.x, 0.38999998569488525390625)) * step(0.20000000298023223876953125, _717.y)) * step(_717.y, 0.660000026226043701171875)) * 0.1500000059604644775390625));
    vec4 _1148;
    _1148.x = _742.x;
    _1148.y = _742.y;
    _1148.z = _742.z;
    vec4 _754 = texture(Texture4, input_uv);
    vec3 _758 = (_754.xyz * 2.0) - vec3(1.0);
    vec2 _760 = fwidth(input_worldPosition.xz);
    vec3 _832 = normalize(normalize(input_normal + (vec3(_758.x, 0.0, _758.y) * (0.100000001490116119384765625 * (1.0 - smoothstep(0.1500000059604644775390625, 1.0, length(_760)))))));
    float _1183;
    do
    {
        if (_128.g_shadowParameters.w <= 0.0)
        {
            _1183 = 1.0;
            break;
        }
        vec4 _939 = _128.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _945 = _939.xyz / vec3(_939.w);
        vec2 _950 = (_945.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_950, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_950, vec2(0.99800002574920654296875)))) || (_945.z < 0.0)) || (_945.z > 1.0))
        {
            _1183 = 1.0;
            break;
        }
        float _978 = _128.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_832, _195.g_sunDirection), 0.0, 1.0))));
        float _1176;
        _1176 = 0.0;
        float _1181;
        SPIRV_CROSS_UNROLL
        for (int _1175 = -1; _1175 <= 1; _1176 = _1181, _1175++)
        {
            _1181 = _1176;
            float _1037;
            SPIRV_CROSS_UNROLL
            for (int _1177 = -1; _1177 <= 1; _1181 = _1037, _1177++)
            {
                vec4 _1003 = textureLod(Texture1, _950 + (vec2(float(_1177), float(_1175)) * _128.g_shadowParameters.xy), 0.0);
                float _1004 = _1003.x;
                float _1179;
                if (_128.g_dynamicShadow.x > 0.5)
                {
                    _1179 = max(_1004, textureLod(Texture3, _950 + (vec2(float(_1177), float(_1175)) * _128.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _1179 = _1004;
                }
                _1037 = _1181 + float(int((_945.z + _978) >= _1179));
            }
        }
        _1183 = mix(1.0, _1176 * 0.111111111938953399658203125, _128.g_shadowParameters.w * clamp(min(min(_950.x, _950.y), min(1.0 - _950.x, 1.0 - _950.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _840 = _195.g_sunColor * _1183;
    _entryPointOutput = vec4(mix(_128.g_fogColorDensity.xyz, ((((_195.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_832.y * 0.5) + 0.5, 0.0, 1.0)))) + (_840 * clamp(dot(_832, _195.g_sunDirection), 0.0, 1.0))) * _1148.xyz) + ((_840 * (_59.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_832, normalize(normalize(_362.g_eyePosition - input_worldPosition) + _195.g_sunDirection)), 0.0, 1.0), max(8.0, _59.g_shininess)) * float(0.0 < dot(_832, _195.g_sunDirection))))) + _59.g_emissionColor, vec3(exp((-_128.g_fogColorDensity.w) * distance(_362.g_eyePosition, input_worldPosition)))), _1173.w);
}


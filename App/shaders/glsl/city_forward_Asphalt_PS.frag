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

vec4 _1188;

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
    vec4 _1176;
    if (_59.g_hasTexture != 0u)
    {
        _1176 = _59.g_diffuseColor * texture(Texture0, input_uv);
    }
    else
    {
        _1176 = _59.g_diffuseColor;
    }
    vec3 _673 = mix(_1176.xyz, vec3(0.9900000095367431640625, 1.0, 1.03499996662139892578125) * dot(_1176.xyz, vec3(0.2125999927520751953125, 0.715200006961822509765625, 0.072200000286102294921875)), vec3(0.87999999523162841796875));
    vec4 _1131;
    _1131.x = _673.x;
    _1131.y = _673.y;
    _1131.z = _673.z;
    vec3 _706 = _1131.xyz * (1.0 + ((sin((input_worldPosition.x * 0.180000007152557373046875) + sin(input_worldPosition.z * 0.2700000107288360595703125)) * sin((input_worldPosition.z * 0.12999999523162841796875) + (input_worldPosition.x * 0.037000000476837158203125))) * 0.085000000894069671630859375));
    vec4 _1141;
    _1141.x = _706.x;
    _1141.y = _706.y;
    _1141.z = _706.z;
    vec2 _720 = fract(input_worldPosition.xz * vec2(0.052631579339504241943359375));
    vec3 _745 = _1141.xyz * (1.0 - (((((step(0.829999983310699462890625, fract(sin(dot(floor(input_worldPosition.xz * vec2(0.052631579339504241943359375)), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875)) * step(0.23000000417232513427734375, _720.x)) * step(_720.x, 0.38999998569488525390625)) * step(0.20000000298023223876953125, _720.y)) * step(_720.y, 0.660000026226043701171875)) * 0.1500000059604644775390625));
    vec4 _1151;
    _1151.x = _745.x;
    _1151.y = _745.y;
    _1151.z = _745.z;
    vec4 _757 = texture(Texture4, input_uv);
    vec3 _761 = (_757.xyz * 2.0) - vec3(1.0);
    vec2 _763 = fwidth(input_worldPosition.xz);
    vec3 _835 = normalize(normalize(input_normal + (vec3(_761.x, 0.0, _761.y) * (0.100000001490116119384765625 * (1.0 - smoothstep(0.1500000059604644775390625, 1.0, length(_763)))))));
    float _1186;
    do
    {
        if (_128.g_shadowParameters.w <= 0.0)
        {
            _1186 = 1.0;
            break;
        }
        vec4 _942 = _128.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _948 = _942.xyz / vec3(_942.w);
        vec2 _953 = (_948.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_953, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_953, vec2(0.99800002574920654296875)))) || (_948.z < 0.0)) || (_948.z > 1.0))
        {
            _1186 = 1.0;
            break;
        }
        float _981 = _128.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_835, _195.g_sunDirection), 0.0, 1.0))));
        float _1179;
        _1179 = 0.0;
        float _1184;
        SPIRV_CROSS_UNROLL
        for (int _1178 = -1; _1178 <= 1; _1179 = _1184, _1178++)
        {
            _1184 = _1179;
            float _1040;
            SPIRV_CROSS_UNROLL
            for (int _1180 = -1; _1180 <= 1; _1184 = _1040, _1180++)
            {
                vec4 _1006 = textureLod(Texture1, _953 + (vec2(float(_1180), float(_1178)) * _128.g_shadowParameters.xy), 0.0);
                float _1007 = _1006.x;
                float _1182;
                if (_128.g_dynamicShadow.x > 0.5)
                {
                    _1182 = max(_1007, textureLod(Texture3, _953 + (vec2(float(_1180), float(_1178)) * _128.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _1182 = _1007;
                }
                _1040 = _1184 + float(int((_948.z + _981) >= _1182));
            }
        }
        _1186 = mix(1.0, _1179 * 0.111111111938953399658203125, _128.g_shadowParameters.w * clamp(min(min(_953.x, _953.y), min(1.0 - _953.x, 1.0 - _953.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _843 = _195.g_sunColor * _1186;
    _entryPointOutput = vec4(mix(_128.g_fogColorDensity.xyz, ((((_195.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_835.y * 0.5) + 0.5, 0.0, 1.0)))) + (_843 * clamp(dot(_835, _195.g_sunDirection), 0.0, 1.0))) * _1151.xyz) + ((_843 * (_59.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_835, normalize(normalize(_362.g_eyePosition - input_worldPosition) + _195.g_sunDirection)), 0.0, 1.0), max(8.0, _59.g_shininess)) * float(0.0 < dot(_835, _195.g_sunDirection))))) + _59.g_emissionColor, vec3(exp((-_128.g_fogColorDensity.w) * distance(_362.g_eyePosition, input_worldPosition)))), _1176.w);
}


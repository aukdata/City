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
} _95;

layout(std140) uniform PSPerFrame
{
    vec3 g_globalAmbientColor;
    vec3 g_sunColor;
    vec3 g_sunDirection;
} _165;

layout(std140) uniform PSPerView
{
    vec3 g_eyePosition;
} _340;

layout(std140) uniform PSPerMaterial
{
    vec3 g_ambientColor;
    uint g_hasTexture;
    vec4 g_diffuseColor;
    vec3 g_specularColor;
    float g_shininess;
    vec3 g_emissionColor;
} _357;

uniform sampler2D Texture1;
uniform sampler2D Texture3;

layout(location = 0) in vec3 input_worldPosition;
layout(location = 1) in vec2 input_uv;
layout(location = 2) in vec3 input_normal;
layout(location = 0) out vec4 _entryPointOutput;

void main()
{
    float _800 = fwidth(input_uv.x * 3.3333332538604736328125);
    float _805 = fwidth(input_uv.y * 3.3333332538604736328125);
    float _809 = 1.0 - smoothstep(0.20000000298023223876953125, 1.2000000476837158203125, max(_800, _805));
    vec2 _819 = input_uv * vec2(3.3333332538604736328125);
    vec2 _932 = input_worldPosition.xz * 0.07500000298023223876953125;
    vec3 _954 = mix(mix(vec3(0.04500000178813934326171875, 0.06800000369548797607421875, 0.05200000107288360595703125) + vec3(sin((input_uv.y * 2.400000095367431640625) + sin(input_uv.x * 0.699999988079071044921875)) * 0.0040000001899898052215576171875), vec3(0.23000000417232513427734375, 0.2899999916553497314453125, 0.310000002384185791015625), vec3(pow(1.0 - clamp(dot(normalize(_340.g_eyePosition - input_worldPosition), normalize(input_normal)), 0.0, 1.0), 4.0) * 0.64999997615814208984375)), mix(vec3(0.07500000298023223876953125, 0.12999999523162841796875, 0.02199999988079071044921875), vec3(0.189999997615814208984375, 0.25, 0.04500000178813934326171875), vec3(mix(0.5, fract(sin(dot(floor(_819), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), 1.0 - smoothstep(0.300000011920928955078125, 1.0, max(fwidth(_819.x), fwidth(_819.y)))))), vec3(mix(0.3499999940395355224609375, smoothstep(0.449999988079071044921875, 0.89999997615814208984375, cos(input_uv.x * 20.943950653076171875)) * smoothstep(-0.25, 0.64999997615814208984375, cos(input_uv.y * 20.943950653076171875)), _809))) * (0.910000026226043701171875 + (0.119999997317790985107421875 * (sin(_932.x + sin(_932.y * 0.62999999523162841796875)) * cos(_932.y + sin(_932.x * 0.4199999868869781494140625)))));
    vec3 _957 = dFdx(input_worldPosition);
    float _960 = dFdy(input_uv.y);
    vec3 _964 = dFdy(input_worldPosition);
    float _967 = dFdx(input_uv.y);
    vec3 _969 = (_957 * _960) - (_964 * _967);
    vec3 _1032 = normalize(normalize(input_normal + ((((_969 / vec3(max(length(_969), 9.9999999747524270787835121154785e-07))) * sin(input_uv.x * 20.943950653076171875)) * 0.119999997317790985107421875) * _809)));
    float _1375;
    do
    {
        if (_95.g_shadowParameters.w <= 0.0)
        {
            _1375 = 1.0;
            break;
        }
        vec4 _1139 = _95.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _1145 = _1139.xyz / vec3(_1139.w);
        vec2 _1150 = (_1145.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_1150, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_1150, vec2(0.99800002574920654296875)))) || (_1145.z < 0.0)) || (_1145.z > 1.0))
        {
            _1375 = 1.0;
            break;
        }
        float _1178 = _95.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_1032, _165.g_sunDirection), 0.0, 1.0))));
        float _1368;
        _1368 = 0.0;
        float _1373;
        SPIRV_CROSS_UNROLL
        for (int _1367 = -1; _1367 <= 1; _1368 = _1373, _1367++)
        {
            _1373 = _1368;
            float _1237;
            SPIRV_CROSS_UNROLL
            for (int _1369 = -1; _1369 <= 1; _1373 = _1237, _1369++)
            {
                vec4 _1203 = textureLod(Texture1, _1150 + (vec2(float(_1369), float(_1367)) * _95.g_shadowParameters.xy), 0.0);
                float _1204 = _1203.x;
                float _1371;
                if (_95.g_dynamicShadow.x > 0.5)
                {
                    _1371 = max(_1204, textureLod(Texture3, _1150 + (vec2(float(_1369), float(_1367)) * _95.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _1371 = _1204;
                }
                _1237 = _1373 + float(int((_1145.z + _1178) >= _1371));
            }
        }
        _1375 = mix(1.0, _1368 * 0.111111111938953399658203125, _95.g_shadowParameters.w * clamp(min(min(_1150.x, _1150.y), min(1.0 - _1150.x, 1.0 - _1150.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _1040 = _165.g_sunColor * _1375;
    vec3 _1106 = mix(_95.g_fogColorDensity.xyz, ((((_165.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_1032.y * 0.5) + 0.5, 0.0, 1.0)))) + (_1040 * clamp(dot(_1032, _165.g_sunDirection), 0.0, 1.0))) * vec4(_954, 1.0).xyz) + ((_1040 * (_357.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_1032, normalize(normalize(_340.g_eyePosition - input_worldPosition) + _165.g_sunDirection)), 0.0, 1.0), max(8.0, _357.g_shininess)) * float(0.0 < dot(_1032, _165.g_sunDirection))))) + _357.g_emissionColor, vec3(exp((-_95.g_fogColorDensity.w) * distance(_340.g_eyePosition, input_worldPosition))));
    _entryPointOutput = vec4(_1106, 1.0);
}


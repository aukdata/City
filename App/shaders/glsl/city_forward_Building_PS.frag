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

const vec3 _491[8] = vec3[](vec3(1.0), vec3(1.0499999523162841796875, 1.08000004291534423828125, 1.0), vec3(1.89999997615814208984375, 1.35000002384185791015625, 0.949999988079071044921875), vec3(2.2000000476837158203125, 1.0499999523162841796875, 0.75), vec3(0.949999988079071044921875, 1.25, 1.0), vec3(2.400000095367431640625, 2.400000095367431640625, 2.2999999523162841796875), vec3(1.0), vec3(1.5, 1.5, 1.4500000476837158203125));
vec4 _1341;

layout(std140) uniform PSPerMaterial
{
    vec3 g_ambientColor;
    uint g_hasTexture;
    vec4 g_diffuseColor;
    vec3 g_specularColor;
    float g_shininess;
    vec3 g_emissionColor;
} _61;

layout(std140) uniform CityParameters
{
    mat4 g_worldToShadow;
    vec4 g_shadowParameters;
    vec4 g_fogColorDensity;
    vec4 g_dynamicShadow;
    vec4 g_altitudeBands;
    vec4 g_terrainVariation;
} _130;

layout(std140) uniform PSPerFrame
{
    vec3 g_globalAmbientColor;
    vec3 g_sunColor;
    vec3 g_sunDirection;
} _197;

layout(std140) uniform PSPerView
{
    vec3 g_eyePosition;
} _364;

layout(std140) uniform BuildingTint
{
    vec4 g_buildingTint;
} _563;

uniform sampler2D Texture0;
uniform sampler2D Texture1;
uniform sampler2D Texture3;

layout(location = 0) in vec3 input_worldPosition;
layout(location = 1) in vec2 input_uv;
layout(location = 2) in vec3 input_normal;
layout(location = 0) out vec4 _entryPointOutput;

void main()
{
    vec2 _708 = input_uv;
    float _764 = floor(_708.x);
    vec2 _1293 = _708;
    _1293.x = _708.x - _764;
    float _775 = (_764 > 0.5) ? _764 : _563.g_buildingTint.x;
    vec4 _1328;
    if (_61.g_hasTexture != 0u)
    {
        _1328 = _61.g_diffuseColor * texture(Texture0, _1293);
    }
    else
    {
        _1328 = _61.g_diffuseColor;
    }
    vec3 _781 = normalize(input_normal);
    vec3 _1329;
    do
    {
        if (_775 < 0.5)
        {
            _1329 = _1328.xyz;
            break;
        }
        float _906 = dot(_1328.xyz, vec3(0.2125999927520751953125, 0.715200006961822509765625, 0.072200000286102294921875));
        int _909 = int(_775) - 1;
        vec3 _926 = _491[_909] * _906;
        bvec3 _935 = bvec3((_909 == 0) || (_909 == 6));
        vec3 _939 = mix(_1328.xyz, vec3(_935.x ? _1328.xyz.x : _926.x, _935.y ? _1328.xyz.y : _926.y, _935.z ? _1328.xyz.z : _926.z), vec3((smoothstep(0.5, 0.699999988079071044921875, _781.y) * (1.0 - smoothstep(0.949999988079071044921875, 0.9900000095367431640625, _781.y))) * (1.0 - smoothstep(0.0599999986588954925537109375, 0.14000000059604644775390625, _906))));
        _1329 = mix(_939, _939 * (mix(vec3(1.03999996185302734375, 1.0, 0.949999988079071044921875), vec3(0.949999988079071044921875, 0.980000019073486328125, 1.0299999713897705078125), vec3(fract(_775 * 0.37000000476837158203125))) * mix(0.920000016689300537109375, 1.08000004291534423828125, fract(_775 * 0.61000001430511474609375))), vec3(1.0 - smoothstep(0.300000011920928955078125, 0.5, abs(_781.y))));
        break;
    } while(false);
    vec4 _1298;
    _1298.x = _1329.x;
    _1298.y = _1329.y;
    _1298.z = _1329.z;
    vec3 _818 = normalize(_364.g_eyePosition - input_worldPosition);
    vec3 _824 = reflect(-_818, normalize(input_normal));
    vec3 _860 = mix(_1298.xyz, mix(vec3(0.12999999523162841796875, 0.1599999964237213134765625, 0.180000007152557373046875), vec3(0.3400000035762786865234375, 0.4699999988079071044921875, 0.589999973773956298828125), vec3(smoothstep(-0.180000007152557373046875, 0.60000002384185791015625, _824.y))) * (0.87999999523162841796875 + (0.119999997317790985107421875 * sin((_824.x * 23.0) + (_824.z * 11.0)))), vec3((((float(_61.g_hasTexture) * smoothstep(0.008000000379979610443115234375, 0.0350000001490116119384765625, _1329.z - _1329.x)) * (1.0 - smoothstep(0.100000001490116119384765625, 0.25, _1329.x))) * (1.0 - smoothstep(0.100000001490116119384765625, 0.4000000059604644775390625, abs(input_normal.y)))) * (0.039999999105930328369140625 + (0.37999999523162841796875 * pow(1.0 - clamp(abs(dot(_818, normalize(input_normal))), 0.0, 1.0), 5.0)))));
    vec4 _1311;
    _1311.x = _860.x;
    _1311.y = _860.y;
    _1311.z = _860.z;
    vec3 _991 = normalize(input_normal);
    float _1339;
    do
    {
        if (_130.g_shadowParameters.w <= 0.0)
        {
            _1339 = 1.0;
            break;
        }
        vec4 _1098 = _130.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _1104 = _1098.xyz / vec3(_1098.w);
        vec2 _1109 = (_1104.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_1109, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_1109, vec2(0.99800002574920654296875)))) || (_1104.z < 0.0)) || (_1104.z > 1.0))
        {
            _1339 = 1.0;
            break;
        }
        float _1137 = _130.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_991, _197.g_sunDirection), 0.0, 1.0))));
        float _1332;
        _1332 = 0.0;
        float _1337;
        SPIRV_CROSS_UNROLL
        for (int _1331 = -1; _1331 <= 1; _1332 = _1337, _1331++)
        {
            _1337 = _1332;
            float _1196;
            SPIRV_CROSS_UNROLL
            for (int _1333 = -1; _1333 <= 1; _1337 = _1196, _1333++)
            {
                vec4 _1162 = textureLod(Texture1, _1109 + (vec2(float(_1333), float(_1331)) * _130.g_shadowParameters.xy), 0.0);
                float _1163 = _1162.x;
                float _1335;
                if (_130.g_dynamicShadow.x > 0.5)
                {
                    _1335 = max(_1163, textureLod(Texture3, _1109 + (vec2(float(_1333), float(_1331)) * _130.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _1335 = _1163;
                }
                _1196 = _1337 + float(int((_1104.z + _1137) >= _1335));
            }
        }
        _1339 = mix(1.0, _1332 * 0.111111111938953399658203125, _130.g_shadowParameters.w * clamp(min(min(_1109.x, _1109.y), min(1.0 - _1109.x, 1.0 - _1109.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _999 = _197.g_sunColor * _1339;
    vec3 _1065 = mix(_130.g_fogColorDensity.xyz, ((((_197.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_991.y * 0.5) + 0.5, 0.0, 1.0)))) + (_999 * clamp(dot(_991, _197.g_sunDirection), 0.0, 1.0))) * _1311.xyz) + ((_999 * (_61.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_991, normalize(normalize(_364.g_eyePosition - input_worldPosition) + _197.g_sunDirection)), 0.0, 1.0), max(8.0, _61.g_shininess)) * float(0.0 < dot(_991, _197.g_sunDirection))))) + _61.g_emissionColor, vec3(exp((-_130.g_fogColorDensity.w) * distance(_364.g_eyePosition, input_worldPosition))));
    _entryPointOutput = vec4(_1065, _1328.w);
}


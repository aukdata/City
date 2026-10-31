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
} _101;

layout(std140) uniform PSPerFrame
{
    vec3 g_globalAmbientColor;
    vec3 g_sunColor;
    vec3 g_sunDirection;
} _171;

layout(std140) uniform PSPerView
{
    vec3 g_eyePosition;
} _346;

layout(std140) uniform PSPerMaterial
{
    vec3 g_ambientColor;
    uint g_hasTexture;
    vec4 g_diffuseColor;
    vec3 g_specularColor;
    float g_shininess;
    vec3 g_emissionColor;
} _363;

uniform sampler2D Texture0;
uniform sampler2D Texture1;
uniform sampler2D Texture3;

layout(location = 0) in vec3 input_worldPosition;
layout(location = 1) in vec2 input_uv;
layout(location = 2) in vec3 input_normal;
layout(location = 0) out vec4 _entryPointOutput;

void main()
{
    vec2 _954 = input_uv;
    float _1040 = floor(_954.x * 0.0009765625);
    float _1046 = _954.x - ((_1040 * 1024.0) + 512.0);
    vec2 _1850 = _954;
    _1850.x = _1046;
    float _1065 = fwidth(_1046 * 1.1764705181121826171875);
    float _1070 = fwidth(_954.y * 1.1764705181121826171875);
    float _1071 = max(_1065, _1070);
    float _1074 = 1.0 - smoothstep(0.20000000298023223876953125, 1.2000000476837158203125, _1071);
    float _1077 = cos(_1046 * 7.391982555389404296875);
    vec2 _1084 = _1850 * vec2(5.5555553436279296875);
    vec3 _1108 = texture(Texture0, _1850 * 0.20999999344348907470703125).xyz;
    bvec3 _1109 = bvec3(_363.g_hasTexture != 0u);
    float _1202 = mix(0.300000011920928955078125, 0.949999988079071044921875, fract(sin(dot(vec2(_1040, 7.69999980926513671875), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875));
    vec2 _1240 = input_worldPosition.xz * 0.07500000298023223876953125;
    vec2 _1272 = (input_worldPosition.xz * vec2(0.0909090936183929443359375)) + vec2(_1040 * 3.7000000476837158203125);
    vec2 _1386 = floor(_1272);
    vec2 _1388 = fract(_1272);
    vec2 _1396 = (_1388 * _1388) * (vec2(3.0) - (_1388 * 2.0));
    vec2 _1281 = (input_worldPosition.xz * vec2(0.23255813121795654296875)) + vec2(_1040 * 1.89999997615814208984375);
    vec2 _1455 = floor(_1281);
    vec2 _1457 = fract(_1281);
    vec2 _1465 = (_1457 * _1457) * (vec2(3.0) - (_1457 * 2.0));
    vec3 _1289 = (mix(mix(vec3(0.085000000894069671630859375, 0.054999999701976776123046875, 0.0320000015199184417724609375), vec3(0.17000000178813934326171875, 0.115000002086162567138671875, 0.06599999964237213134765625), vec3(clamp(dot(vec3(_1109.x ? _1108.x : vec3(0.3499999940395355224609375, 0.300000011920928955078125, 0.20000000298023223876953125).x, _1109.y ? _1108.y : vec3(0.3499999940395355224609375, 0.300000011920928955078125, 0.20000000298023223876953125).y, _1109.z ? _1108.z : vec3(0.3499999940395355224609375, 0.300000011920928955078125, 0.20000000298023223876953125).z), vec3(0.300000011920928955078125, 0.5, 0.20000000298023223876953125)) * 2.0, 0.0, 1.0))) * mix(1.0, 0.75 + (0.319999992847442626953125 * ((_1077 * 0.5) + 0.5)), _1074), mix(mix(vec3(0.0500000007450580596923828125, 0.0949999988079071044921875, 0.0199999995529651641845703125), vec3(0.119999997317790985107421875, 0.17000000178813934326171875, 0.0350000001490116119384765625), vec3(mix(0.5, fract(sin(dot(floor(_1084), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), 1.0 - smoothstep(0.300000011920928955078125, 1.0, max(fwidth(_1084.x), fwidth(_1084.y)))))), vec3(0.0350000001490116119384765625, 0.070000000298023223876953125, 0.0199999995529651641845703125), vec3(step(0.60000002384185791015625, fract(sin(dot(vec2((_1040 * 1.37000000476837158203125) + 0.5, 11.30000019073486328125), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875)) * 0.60000002384185791015625)), vec3(mix(_1202, (smoothstep(0.89999997615814208984375 - (_1202 * 1.2999999523162841796875), 1.2000000476837158203125 - (_1202 * 1.2999999523162841796875), _1077) * smoothstep(-0.60000002384185791015625, 0.20000000298023223876953125, cos(_954.y * 7.400000095367431640625) + _1202)) * mix(0.5, 1.0, _1202), _1074))) * (0.930000007152557373046875 + (0.0900000035762786865234375 * (sin(_1240.x + sin(_1240.y * 0.62999999523162841796875)) * cos(_1240.y + sin(_1240.x * 0.4199999868869781494140625)))))) * (0.86000001430511474609375 + (0.2599999904632568359375 * ((mix(mix(fract(sin(dot(_1386, vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_1386 + vec2(1.0, 0.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _1396.x), mix(fract(sin(dot(_1386 + vec2(0.0, 1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_1386 + vec2(1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _1396.x), _1396.y) * 0.60000002384185791015625) + (mix(mix(fract(sin(dot(_1455, vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_1455 + vec2(1.0, 0.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _1465.x), mix(fract(sin(dot(_1455 + vec2(0.0, 1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_1455 + vec2(1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _1465.x), _1465.y) * 0.4000000059604644775390625))));
    vec3 _1305 = _1289 * (1.0 + ((smoothstep(0.20000000298023223876953125, 1.2000000476837158203125, _1071) * 0.02500000037252902984619140625) * sin(_1046 * 0.89999997615814208984375)));
    vec3 _1315 = dFdx(input_worldPosition);
    float _1318 = dFdy(_954.y);
    vec3 _1322 = dFdy(input_worldPosition);
    float _1325 = dFdx(_954.y);
    vec3 _1327 = (_1315 * _1318) - (_1322 * _1325);
    vec3 _1542 = normalize(normalize(input_normal + ((((_1327 / vec3(max(length(_1327), 9.9999999747524270787835121154785e-07))) * sin(_1046 * 7.391982555389404296875)) * 0.4799999892711639404296875) * _1074)));
    float _1909;
    do
    {
        if (_101.g_shadowParameters.w <= 0.0)
        {
            _1909 = 1.0;
            break;
        }
        vec4 _1649 = _101.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _1655 = _1649.xyz / vec3(_1649.w);
        vec2 _1660 = (_1655.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_1660, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_1660, vec2(0.99800002574920654296875)))) || (_1655.z < 0.0)) || (_1655.z > 1.0))
        {
            _1909 = 1.0;
            break;
        }
        float _1688 = _101.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_1542, _171.g_sunDirection), 0.0, 1.0))));
        float _1902;
        _1902 = 0.0;
        float _1907;
        SPIRV_CROSS_UNROLL
        for (int _1901 = -1; _1901 <= 1; _1902 = _1907, _1901++)
        {
            _1907 = _1902;
            float _1747;
            SPIRV_CROSS_UNROLL
            for (int _1903 = -1; _1903 <= 1; _1907 = _1747, _1903++)
            {
                vec4 _1713 = textureLod(Texture1, _1660 + (vec2(float(_1903), float(_1901)) * _101.g_shadowParameters.xy), 0.0);
                float _1714 = _1713.x;
                float _1905;
                if (_101.g_dynamicShadow.x > 0.5)
                {
                    _1905 = max(_1714, textureLod(Texture3, _1660 + (vec2(float(_1903), float(_1901)) * _101.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _1905 = _1714;
                }
                _1747 = _1907 + float(int((_1655.z + _1688) >= _1905));
            }
        }
        _1909 = mix(1.0, _1902 * 0.111111111938953399658203125, _101.g_shadowParameters.w * clamp(min(min(_1660.x, _1660.y), min(1.0 - _1660.x, 1.0 - _1660.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _1550 = _171.g_sunColor * _1909;
    vec3 _1616 = mix(_101.g_fogColorDensity.xyz, ((((_171.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_1542.y * 0.5) + 0.5, 0.0, 1.0)))) + (_1550 * clamp(dot(_1542, _171.g_sunDirection), 0.0, 1.0))) * vec4(mix(vec3(dot(_1305, vec3(0.2125999927520751953125, 0.715200006961822509765625, 0.072200000286102294921875))), _1305, vec3(0.85000002384185791015625)), 1.0).xyz) + ((_1550 * (_363.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_1542, normalize(normalize(_346.g_eyePosition - input_worldPosition) + _171.g_sunDirection)), 0.0, 1.0), max(8.0, _363.g_shininess)) * float(0.0 < dot(_1542, _171.g_sunDirection))))) + _363.g_emissionColor, vec3(exp((-_101.g_fogColorDensity.w) * distance(_346.g_eyePosition, input_worldPosition))));
    _entryPointOutput = vec4(_1616, 1.0);
}


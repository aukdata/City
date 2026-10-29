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
    vec2 _830 = input_uv;
    float _910 = floor(_830.x * 0.0009765625);
    float _916 = _830.x - ((_910 * 1024.0) + 512.0);
    vec2 _1532 = _830;
    _1532.x = _916;
    float _1189 = fract(sin(dot(vec2((_910 * 1.37000000476837158203125) + 0.5, 11.30000019073486328125), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875);
    float _935 = fwidth(_916 * 3.3333332538604736328125);
    float _940 = fwidth(_830.y * 3.3333332538604736328125);
    float _944 = 1.0 - smoothstep(0.20000000298023223876953125, 1.2000000476837158203125, max(_935, _940));
    vec2 _954 = _1532 * vec2(3.3333332538604736328125);
    vec2 _1110 = input_worldPosition.xz * 0.07500000298023223876953125;
    vec3 _1132 = mix(mix(vec3(0.04500000178813934326171875, 0.06800000369548797607421875, 0.05200000107288360595703125) + vec3(sin((_830.y * 2.400000095367431640625) + sin(_916 * 0.699999988079071044921875)) * 0.0040000001899898052215576171875), vec3(0.23000000417232513427734375, 0.2899999916553497314453125, 0.310000002384185791015625), vec3(pow(1.0 - clamp(dot(normalize(_340.g_eyePosition - input_worldPosition), normalize(input_normal)), 0.0, 1.0), 4.0) * 0.64999997615814208984375)), mix(mix(vec3(0.07400000095367431640625, 0.1500000059604644775390625, 0.0199999995529651641845703125), vec3(0.122000001370906829833984375, 0.20499999821186065673828125, 0.0280000008642673492431640625), vec3(fract(sin(dot(vec2(_910, 3.099999904632568359375), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875))), vec3(0.16500000655651092529296875, 0.189999997615814208984375, 0.039999999105930328369140625), vec3(smoothstep(0.75, 0.949999988079071044921875, _1189) * 0.800000011920928955078125)) * mix(0.87999999523162841796875, 1.12000000476837158203125, mix(0.5, fract(sin(dot(floor(_954), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), 1.0 - smoothstep(0.300000011920928955078125, 1.0, max(fwidth(_954.x), fwidth(_954.y))))), vec3(mix(mix(0.819999992847442626953125, 0.550000011920928955078125, step(0.89999997615814208984375, _1189)), clamp(((smoothstep(0.449999988079071044921875, 0.89999997615814208984375, cos(_916 * 20.943950653076171875)) * smoothstep(-0.25, 0.64999997615814208984375, cos(_830.y * 20.943950653076171875))) * 0.60000002384185791015625) + 0.449999988079071044921875, 0.0, 1.0), _944))) * (0.930000007152557373046875 + (0.0900000035762786865234375 * (sin(_1110.x + sin(_1110.y * 0.62999999523162841796875)) * cos(_1110.y + sin(_1110.x * 0.4199999868869781494140625)))));
    vec3 _1135 = dFdx(input_worldPosition);
    float _1138 = dFdy(_830.y);
    vec3 _1142 = dFdy(input_worldPosition);
    float _1145 = dFdx(_830.y);
    vec3 _1147 = (_1135 * _1138) - (_1142 * _1145);
    vec3 _1224 = normalize(normalize(input_normal + ((((_1147 / vec3(max(length(_1147), 9.9999999747524270787835121154785e-07))) * sin(_916 * 20.943950653076171875)) * 0.119999997317790985107421875) * _944)));
    float _1572;
    do
    {
        if (_95.g_shadowParameters.w <= 0.0)
        {
            _1572 = 1.0;
            break;
        }
        vec4 _1331 = _95.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _1337 = _1331.xyz / vec3(_1331.w);
        vec2 _1342 = (_1337.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_1342, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_1342, vec2(0.99800002574920654296875)))) || (_1337.z < 0.0)) || (_1337.z > 1.0))
        {
            _1572 = 1.0;
            break;
        }
        float _1370 = _95.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_1224, _165.g_sunDirection), 0.0, 1.0))));
        float _1565;
        _1565 = 0.0;
        float _1570;
        SPIRV_CROSS_UNROLL
        for (int _1564 = -1; _1564 <= 1; _1565 = _1570, _1564++)
        {
            _1570 = _1565;
            float _1429;
            SPIRV_CROSS_UNROLL
            for (int _1566 = -1; _1566 <= 1; _1570 = _1429, _1566++)
            {
                vec4 _1395 = textureLod(Texture1, _1342 + (vec2(float(_1566), float(_1564)) * _95.g_shadowParameters.xy), 0.0);
                float _1396 = _1395.x;
                float _1568;
                if (_95.g_dynamicShadow.x > 0.5)
                {
                    _1568 = max(_1396, textureLod(Texture3, _1342 + (vec2(float(_1566), float(_1564)) * _95.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _1568 = _1396;
                }
                _1429 = _1570 + float(int((_1337.z + _1370) >= _1568));
            }
        }
        _1572 = mix(1.0, _1565 * 0.111111111938953399658203125, _95.g_shadowParameters.w * clamp(min(min(_1342.x, _1342.y), min(1.0 - _1342.x, 1.0 - _1342.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _1232 = _165.g_sunColor * _1572;
    vec3 _1298 = mix(_95.g_fogColorDensity.xyz, ((((_165.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_1224.y * 0.5) + 0.5, 0.0, 1.0)))) + (_1232 * clamp(dot(_1224, _165.g_sunDirection), 0.0, 1.0))) * vec4(_1132, 1.0).xyz) + ((_1232 * (_357.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_1224, normalize(normalize(_340.g_eyePosition - input_worldPosition) + _165.g_sunDirection)), 0.0, 1.0), max(8.0, _357.g_shininess)) * float(0.0 < dot(_1224, _165.g_sunDirection))))) + _357.g_emissionColor, vec3(exp((-_95.g_fogColorDensity.w) * distance(_340.g_eyePosition, input_worldPosition))));
    _entryPointOutput = vec4(_1298, 1.0);
}


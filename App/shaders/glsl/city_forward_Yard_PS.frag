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
} _92;

layout(std140) uniform PSPerFrame
{
    vec3 g_globalAmbientColor;
    vec3 g_sunColor;
    vec3 g_sunDirection;
} _162;

layout(std140) uniform PSPerView
{
    vec3 g_eyePosition;
} _337;

layout(std140) uniform PSPerMaterial
{
    vec3 g_ambientColor;
    uint g_hasTexture;
    vec4 g_diffuseColor;
    vec3 g_specularColor;
    float g_shininess;
    vec3 g_emissionColor;
} _354;

uniform sampler2D Texture0;
uniform sampler2D Texture1;
uniform sampler2D Texture3;

layout(location = 0) in vec3 input_worldPosition;
layout(location = 1) in vec2 input_uv;
layout(location = 2) in vec3 input_normal;
layout(location = 0) out vec4 _entryPointOutput;

void main()
{
    float _750 = 1.0 - _354.g_diffuseColor.w;
    vec2 _755 = input_uv * vec2(0.3225806653499603271484375, 0.43478262424468994140625);
    vec2 _757 = fract(_755);
    vec2 _790 = (input_worldPosition.xz * vec2(0.15384615957736968994140625)) + vec2(13.69999980926513671875);
    vec2 _879 = floor(_790);
    vec2 _881 = fract(_790);
    vec2 _889 = (_881 * _881) * (vec2(3.0) - (_881 * 2.0));
    vec2 _816 = (input_worldPosition.xz * vec2(0.23255813121795654296875)) + vec2(2.900000095367431640625);
    vec2 _948 = floor(_816);
    vec2 _950 = fract(_816);
    vec2 _958 = (_950 * _950) * (vec2(3.0) - (_950 * 2.0));
    vec2 _828 = (input_worldPosition.xz * vec2(0.625)) + vec2(5.099999904632568359375);
    vec2 _1017 = floor(_828);
    vec2 _1019 = fract(_828);
    vec2 _1027 = (_1019 * _1019) * (vec2(3.0) - (_1019 * 2.0));
    vec3 _842 = vec3(mix(mix(0.62000000476837158203125, 0.2800000011920928955078125, _750), smoothstep(0.36000001430511474609375, 0.5, (((fract(sin(dot(floor(_755) + (floor(input_worldPosition.xz * vec2(0.02500000037252902984619140625)) * 7.30000019073486328125), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875) * mix(0.60000002384185791015625, 1.0, smoothstep(0.0, 0.119999997317790985107421875, min(min(_757.x, 1.0 - _757.x), min(_757.y, 1.0 - _757.y))))) * 0.800000011920928955078125) + (mix(mix(fract(sin(dot(_879, vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_879 + vec2(1.0, 0.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _889.x), mix(fract(sin(dot(_879 + vec2(0.0, 1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_879 + vec2(1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _889.x), _889.y) * 0.20000000298023223876953125)) - (_750 * 0.4000000059604644775390625)), 1.0 - smoothstep(0.5, 2.0, length(fwidth(input_uv)))));
    vec2 _850 = (input_worldPosition.xz * vec2(0.2439024448394775390625)) + vec2(29.299999237060546875);
    vec2 _1086 = floor(_850);
    vec2 _1088 = fract(_850);
    vec2 _1096 = (_1088 * _1088) * (vec2(3.0) - (_1088 * 2.0));
    vec3 _855 = mix(mix(mix(vec3(0.064000003039836883544921875, 0.097999997437000274658203125, 0.02999999932944774627685546875) * (0.920000016689300537109375 + (mix(mix(fract(sin(dot(_948, vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_948 + vec2(1.0, 0.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _958.x), mix(fract(sin(dot(_948 + vec2(0.0, 1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_948 + vec2(1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _958.x), _958.y) * 0.1599999964237213134765625)), vec3(0.026000000536441802978515625, 0.0460000000894069671630859375, 0.017999999225139617919921875), vec3(smoothstep(0.62000000476837158203125, 0.819999992847442626953125, mix(mix(fract(sin(dot(_1017, vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_1017 + vec2(1.0, 0.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _1027.x), mix(fract(sin(dot(_1017 + vec2(0.0, 1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_1017 + vec2(1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _1027.x), _1027.y)))), vec3(0.119999997317790985107421875, 0.1180000007152557373046875, 0.11200000345706939697265625) * mix(1.0, clamp(dot(texture(Texture0, input_worldPosition.xz * vec2(0.3333333432674407958984375)).xyz, vec3(0.300000011920928955078125, 0.60000002384185791015625, 0.100000001490116119384765625)) * 4.545454502105712890625, 0.699999988079071044921875, 1.2999999523162841796875), 0.60000002384185791015625), _842), vec3(0.10999999940395355224609375, 0.092000000178813934326171875, 0.06599999964237213134765625), vec3(smoothstep(0.7200000286102294921875, 0.89999997615814208984375, mix(mix(fract(sin(dot(_1086, vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_1086 + vec2(1.0, 0.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _1096.x), mix(fract(sin(dot(_1086 + vec2(0.0, 1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_1086 + vec2(1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _1096.x), _1096.y)) * 0.4000000059604644775390625));
    vec3 _1173 = normalize(input_normal);
    float _1531;
    do
    {
        if (_92.g_shadowParameters.w <= 0.0)
        {
            _1531 = 1.0;
            break;
        }
        vec4 _1280 = _92.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _1286 = _1280.xyz / vec3(_1280.w);
        vec2 _1291 = (_1286.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_1291, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_1291, vec2(0.99800002574920654296875)))) || (_1286.z < 0.0)) || (_1286.z > 1.0))
        {
            _1531 = 1.0;
            break;
        }
        float _1319 = _92.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_1173, _162.g_sunDirection), 0.0, 1.0))));
        float _1524;
        _1524 = 0.0;
        float _1529;
        SPIRV_CROSS_UNROLL
        for (int _1523 = -1; _1523 <= 1; _1524 = _1529, _1523++)
        {
            _1529 = _1524;
            float _1378;
            SPIRV_CROSS_UNROLL
            for (int _1525 = -1; _1525 <= 1; _1529 = _1378, _1525++)
            {
                vec4 _1344 = textureLod(Texture1, _1291 + (vec2(float(_1525), float(_1523)) * _92.g_shadowParameters.xy), 0.0);
                float _1345 = _1344.x;
                float _1527;
                if (_92.g_dynamicShadow.x > 0.5)
                {
                    _1527 = max(_1345, textureLod(Texture3, _1291 + (vec2(float(_1525), float(_1523)) * _92.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _1527 = _1345;
                }
                _1378 = _1529 + float(int((_1286.z + _1319) >= _1527));
            }
        }
        _1531 = mix(1.0, _1524 * 0.111111111938953399658203125, _92.g_shadowParameters.w * clamp(min(min(_1291.x, _1291.y), min(1.0 - _1291.x, 1.0 - _1291.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _1181 = _162.g_sunColor * _1531;
    vec3 _1247 = mix(_92.g_fogColorDensity.xyz, ((((_162.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_1173.y * 0.5) + 0.5, 0.0, 1.0)))) + (_1181 * clamp(dot(_1173, _162.g_sunDirection), 0.0, 1.0))) * vec4(_855, 1.0).xyz) + ((_1181 * (_354.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_1173, normalize(normalize(_337.g_eyePosition - input_worldPosition) + _162.g_sunDirection)), 0.0, 1.0), max(8.0, _354.g_shininess)) * float(0.0 < dot(_1173, _162.g_sunDirection))))) + _354.g_emissionColor, vec3(exp((-_92.g_fogColorDensity.w) * distance(_337.g_eyePosition, input_worldPosition))));
    _entryPointOutput = vec4(_1247, 1.0);
}


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
    vec2 _731 = input_uv * vec2(0.3225806653499603271484375, 0.43478262424468994140625);
    vec2 _733 = fract(_731);
    vec2 _768 = (input_worldPosition.xz * vec2(0.15384615957736968994140625)) + vec2(13.69999980926513671875);
    vec2 _842 = floor(_768);
    vec2 _844 = fract(_768);
    vec2 _852 = (_844 * _844) * (vec2(3.0) - (_844 * 2.0));
    vec2 _796 = (input_worldPosition.xz * vec2(0.769230782985687255859375)) + vec2(5.099999904632568359375);
    vec2 _911 = floor(_796);
    vec2 _913 = fract(_796);
    vec2 _921 = (_913 * _913) * (vec2(3.0) - (_913 * 2.0));
    float _804 = smoothstep(0.36000001430511474609375, 0.5, ((mix(0.5, fract(sin(dot(floor(_731) + (floor(input_worldPosition.xz * vec2(0.02500000037252902984619140625)) * 7.30000019073486328125), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), (1.0 - smoothstep(0.5, 2.0, length(fwidth(input_uv)))) * mix(0.60000002384185791015625, 1.0, smoothstep(0.0, 0.119999997317790985107421875, min(min(_733.x, 1.0 - _733.x), min(_733.y, 1.0 - _733.y))))) * 0.75) + (mix(mix(fract(sin(dot(_842, vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_842 + vec2(1.0, 0.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _852.x), mix(fract(sin(dot(_842 + vec2(0.0, 1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_842 + vec2(1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _852.x), _852.y) * 0.25)) - ((1.0 - _354.g_diffuseColor.w) * 0.4000000059604644775390625));
    vec2 _813 = (input_worldPosition.xz * vec2(0.2439024448394775390625)) + vec2(29.299999237060546875);
    vec2 _980 = floor(_813);
    vec2 _982 = fract(_813);
    vec2 _990 = (_982 * _982) * (vec2(3.0) - (_982 * 2.0));
    vec3 _818 = mix(mix(mix(vec3(0.026000000536441802978515625, 0.0460000000894069671630859375, 0.017999999225139617919921875), vec3(0.064000003039836883544921875, 0.097999997437000274658203125, 0.02999999932944774627685546875), vec3(smoothstep(0.25, 0.64999997615814208984375, mix(mix(fract(sin(dot(_911, vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_911 + vec2(1.0, 0.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _921.x), mix(fract(sin(dot(_911 + vec2(0.0, 1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_911 + vec2(1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _921.x), _921.y)))), vec3(0.119999997317790985107421875, 0.1180000007152557373046875, 0.11200000345706939697265625) * mix(1.0, clamp(dot(texture(Texture0, input_worldPosition.xz * vec2(0.3333333432674407958984375)).xyz, vec3(0.300000011920928955078125, 0.60000002384185791015625, 0.100000001490116119384765625)) * 4.545454502105712890625, 0.699999988079071044921875, 1.2999999523162841796875), 0.60000002384185791015625), vec3(_804)), vec3(0.10999999940395355224609375, 0.092000000178813934326171875, 0.06599999964237213134765625), vec3(smoothstep(0.7200000286102294921875, 0.89999997615814208984375, mix(mix(fract(sin(dot(_980, vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_980 + vec2(1.0, 0.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _990.x), mix(fract(sin(dot(_980 + vec2(0.0, 1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_980 + vec2(1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _990.x), _990.y)) * 0.4000000059604644775390625));
    vec3 _1067 = normalize(input_normal);
    float _1418;
    do
    {
        if (_92.g_shadowParameters.w <= 0.0)
        {
            _1418 = 1.0;
            break;
        }
        vec4 _1174 = _92.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _1180 = _1174.xyz / vec3(_1174.w);
        vec2 _1185 = (_1180.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_1185, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_1185, vec2(0.99800002574920654296875)))) || (_1180.z < 0.0)) || (_1180.z > 1.0))
        {
            _1418 = 1.0;
            break;
        }
        float _1213 = _92.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_1067, _162.g_sunDirection), 0.0, 1.0))));
        float _1411;
        _1411 = 0.0;
        float _1416;
        SPIRV_CROSS_UNROLL
        for (int _1410 = -1; _1410 <= 1; _1411 = _1416, _1410++)
        {
            _1416 = _1411;
            float _1272;
            SPIRV_CROSS_UNROLL
            for (int _1412 = -1; _1412 <= 1; _1416 = _1272, _1412++)
            {
                vec4 _1238 = textureLod(Texture1, _1185 + (vec2(float(_1412), float(_1410)) * _92.g_shadowParameters.xy), 0.0);
                float _1239 = _1238.x;
                float _1414;
                if (_92.g_dynamicShadow.x > 0.5)
                {
                    _1414 = max(_1239, textureLod(Texture3, _1185 + (vec2(float(_1412), float(_1410)) * _92.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _1414 = _1239;
                }
                _1272 = _1416 + float(int((_1180.z + _1213) >= _1414));
            }
        }
        _1418 = mix(1.0, _1411 * 0.111111111938953399658203125, _92.g_shadowParameters.w * clamp(min(min(_1185.x, _1185.y), min(1.0 - _1185.x, 1.0 - _1185.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _1075 = _162.g_sunColor * _1418;
    vec3 _1141 = mix(_92.g_fogColorDensity.xyz, ((((_162.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_1067.y * 0.5) + 0.5, 0.0, 1.0)))) + (_1075 * clamp(dot(_1067, _162.g_sunDirection), 0.0, 1.0))) * vec4(_818, 1.0).xyz) + ((_1075 * (_354.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_1067, normalize(normalize(_337.g_eyePosition - input_worldPosition) + _162.g_sunDirection)), 0.0, 1.0), max(8.0, _354.g_shininess)) * float(0.0 < dot(_1067, _162.g_sunDirection))))) + _354.g_emissionColor, vec3(exp((-_92.g_fogColorDensity.w) * distance(_337.g_eyePosition, input_worldPosition))));
    _entryPointOutput = vec4(_1141, 1.0);
}


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

vec4 _1330;

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
uniform sampler2D Texture2;
uniform sampler2D Texture1;
uniform sampler2D Texture3;

layout(location = 0) in vec3 input_worldPosition;
layout(location = 1) in vec2 input_uv;
layout(location = 2) in vec3 input_normal;
layout(location = 0) out vec4 _entryPointOutput;

void main()
{
    vec4 _1318;
    if (_55.g_hasTexture != 0u)
    {
        _1318 = _55.g_diffuseColor * texture(Texture0, input_uv);
    }
    else
    {
        _1318 = _55.g_diffuseColor;
    }
    vec4 _1319;
    if (_55.g_hasTexture != 0u)
    {
        vec3 _765 = mix(_1318.xyz, _55.g_diffuseColor.xyz * texture(Texture0, (vec2(-input_uv.y, input_uv.x) * 0.17299999296665191650390625) + vec2(0.310000002384185791015625, 0.670000016689300537109375)).xyz, vec3(0.4199999868869781494140625));
        vec4 _1267 = _1318;
        _1267.x = _765.x;
        _1267.y = _765.y;
        _1267.z = _765.z;
        _1319 = _1267;
    }
    else
    {
        _1319 = _1318;
    }
    vec3 _789 = mix(texture(Texture2, input_uv).xyz * vec3(0.7599999904632568359375, 0.699999988079071044921875, 0.550000011920928955078125), _1319.xyz, vec3(smoothstep(0.25, 3.0, input_worldPosition.y)));
    vec4 _1274;
    _1274.x = _789.x;
    _1274.y = _789.y;
    _1274.z = _789.z;
    vec3 _825 = _1274.xyz * mix(vec3(1.0), vec3(0.959999978542327880859375, 0.800000011920928955078125, 0.689999997615814208984375), vec3(smoothstep(0.0500000007450580596923828125, 0.7200000286102294921875, sin((input_worldPosition.x * 0.006000000052154064178466796875) + sin(input_worldPosition.z * 0.0040000001899898052215576171875)) * sin((input_worldPosition.z * 0.004999999888241291046142578125) + (input_worldPosition.x * 0.00200000009499490261077880859375))) * 0.4199999868869781494140625));
    vec4 _1284;
    _1284.x = _825.x;
    _1284.y = _825.y;
    _1284.z = _825.z;
    float _845 = sin((input_worldPosition.x * 0.008000000379979610443115234375) + sin(input_worldPosition.z * 0.006000000052154064178466796875)) * sin(input_worldPosition.z * 0.0089999996125698089599609375);
    float _852 = input_worldPosition.y + (_845 * _124.g_terrainVariation.x);
    float _858 = smoothstep(_124.g_altitudeBands.x, _124.g_altitudeBands.y, _852);
    float _864 = 1.0 - clamp(normalize(input_normal).y, 0.0, 1.0);
    vec3 _884 = mix(_1284.xyz, mix(vec3(0.25, 0.2899999916553497314453125, 0.1599999964237213134765625), vec3(0.3499999940395355224609375, 0.3400000035762786865234375, 0.319999992847442626953125), vec3(clamp((_864 * 2.0) + (_858 * 0.5), 0.0, 1.0))) * (0.800000011920928955078125 + dot(_1284.xyz, vec3(0.2125999927520751953125, 0.715200006961822509765625, 0.072200000286102294921875))), vec3(_858));
    vec4 _1294;
    _1294.x = _884.x;
    _1294.y = _884.y;
    _1294.z = _884.z;
    vec3 _910 = mix(_1294.xyz, vec3(0.86000001430511474609375, 0.89999997615814208984375, 0.949999988079071044921875) * (1.0 + (_845 * 0.0350000001490116119384765625)), vec3(smoothstep(_124.g_altitudeBands.z, _124.g_altitudeBands.w, _852) * (1.0 - smoothstep(0.550000011920928955078125, 0.85000002384185791015625, _864))));
    vec4 _1300;
    _1300.x = _910.x;
    _1300.y = _910.y;
    _1300.z = _910.z;
    vec3 _965 = normalize(input_normal);
    float _1328;
    do
    {
        if (_124.g_shadowParameters.w <= 0.0)
        {
            _1328 = 1.0;
            break;
        }
        vec4 _1072 = _124.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _1078 = _1072.xyz / vec3(_1072.w);
        vec2 _1083 = (_1078.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_1083, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_1083, vec2(0.99800002574920654296875)))) || (_1078.z < 0.0)) || (_1078.z > 1.0))
        {
            _1328 = 1.0;
            break;
        }
        float _1111 = _124.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_965, _191.g_sunDirection), 0.0, 1.0))));
        float _1321;
        _1321 = 0.0;
        float _1326;
        SPIRV_CROSS_UNROLL
        for (int _1320 = -1; _1320 <= 1; _1321 = _1326, _1320++)
        {
            _1326 = _1321;
            float _1170;
            SPIRV_CROSS_UNROLL
            for (int _1322 = -1; _1322 <= 1; _1326 = _1170, _1322++)
            {
                vec4 _1136 = textureLod(Texture1, _1083 + (vec2(float(_1322), float(_1320)) * _124.g_shadowParameters.xy), 0.0);
                float _1137 = _1136.x;
                float _1324;
                if (_124.g_dynamicShadow.x > 0.5)
                {
                    _1324 = max(_1137, textureLod(Texture3, _1083 + (vec2(float(_1322), float(_1320)) * _124.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _1324 = _1137;
                }
                _1170 = _1326 + float(int((_1078.z + _1111) >= _1324));
            }
        }
        _1328 = mix(1.0, _1321 * 0.111111111938953399658203125, _124.g_shadowParameters.w * clamp(min(min(_1083.x, _1083.y), min(1.0 - _1083.x, 1.0 - _1083.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _973 = _191.g_sunColor * _1328;
    vec3 _1039 = mix(_124.g_fogColorDensity.xyz, ((((_191.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_965.y * 0.5) + 0.5, 0.0, 1.0)))) + (_973 * clamp(dot(_965, _191.g_sunDirection), 0.0, 1.0))) * _1300.xyz) + ((_973 * (_55.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_965, normalize(normalize(_358.g_eyePosition - input_worldPosition) + _191.g_sunDirection)), 0.0, 1.0), max(8.0, _55.g_shininess)) * float(0.0 < dot(_965, _191.g_sunDirection))))) + _55.g_emissionColor, vec3(exp((-_124.g_fogColorDensity.w) * distance(_358.g_eyePosition, input_worldPosition))));
    _entryPointOutput = vec4(_1039, _1319.w);
}


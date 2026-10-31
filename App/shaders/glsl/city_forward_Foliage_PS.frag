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
} _100;

layout(std140) uniform PSPerFrame
{
    vec3 g_globalAmbientColor;
    vec3 g_sunColor;
    vec3 g_sunDirection;
} _170;

layout(std140) uniform PSPerView
{
    vec3 g_eyePosition;
} _345;

layout(std140) uniform PSPerMaterial
{
    vec3 g_ambientColor;
    uint g_hasTexture;
    vec4 g_diffuseColor;
    vec3 g_specularColor;
    float g_shininess;
    vec3 g_emissionColor;
} _362;

uniform sampler2D Texture1;
uniform sampler2D Texture3;

layout(location = 0) in vec3 input_worldPosition;
layout(location = 2) in vec3 input_normal;
layout(location = 0) out vec4 _entryPointOutput;

void main()
{
    vec3 _751 = input_worldPosition * 18.0;
    vec3 _753 = fwidth(_751);
    float _756 = 1.0 - smoothstep(0.60000002384185791015625, 2.0, length(_753));
    vec3 _794 = (_362.g_diffuseColor.xyz * mix(0.800000011920928955078125, 1.10000002384185791015625, clamp(0.449999988079071044921875 + ((sin((input_worldPosition.x * 0.730000019073486328125) + sin(input_worldPosition.z * 0.910000026226043701171875)) * cos(input_worldPosition.y * 1.37000000476837158203125)) * 0.300000011920928955078125), 0.0, 1.0))) * mix(1.0, 0.579999983310699462890625 + (fract(sin(dot(floor(_751.xz + vec2(_751.y * 0.430000007152557373046875)), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875) * 0.839999973773956298828125), _756);
    vec3 _1347;
    do
    {
        if (_100.g_terrainVariation.y <= 0.0)
        {
            _1347 = _794;
            break;
        }
        vec2 _862 = (input_worldPosition.xz * vec2(0.011111111380159854888916015625)) + vec2(7.099999904632568359375);
        vec2 _926 = floor(_862);
        vec2 _928 = fract(_862);
        vec2 _936 = (_928 * _928) * (vec2(3.0) - (_928 * 2.0));
        float _992 = fract(sin(dot(floor(input_worldPosition.xz * vec2(0.181818187236785888671875)) + vec2(19.700000762939453125), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875);
        bvec3 _888 = bvec3(_992 < 0.839999973773956298828125);
        vec3 _889 = vec3(_888.x ? vec3(0.519999980926513671875, 0.119999997317790985107421875, 0.0599999986588954925537109375).x : vec3(0.36000001430511474609375, 0.23999999463558197021484375, 0.100000001490116119384765625).x, _888.y ? vec3(0.519999980926513671875, 0.119999997317790985107421875, 0.0599999986588954925537109375).y : vec3(0.36000001430511474609375, 0.23999999463558197021484375, 0.100000001490116119384765625).y, _888.z ? vec3(0.519999980926513671875, 0.119999997317790985107421875, 0.0599999986588954925537109375).z : vec3(0.36000001430511474609375, 0.23999999463558197021484375, 0.100000001490116119384765625).z);
        bvec3 _890 = bvec3(_992 < 0.660000026226043701171875);
        vec3 _891 = vec3(_890.x ? vec3(0.660000026226043701171875, 0.300000011920928955078125, 0.070000000298023223876953125).x : _889.x, _890.y ? vec3(0.660000026226043701171875, 0.300000011920928955078125, 0.070000000298023223876953125).y : _889.y, _890.z ? vec3(0.660000026226043701171875, 0.300000011920928955078125, 0.070000000298023223876953125).z : _889.z);
        bvec3 _892 = bvec3(_992 < 0.37999999523162841796875);
        _1347 = mix(_794, (vec3(_892.x ? vec3(0.62000000476837158203125, 0.4799999892711639404296875, 0.100000001490116119384765625).x : _891.x, _892.y ? vec3(0.62000000476837158203125, 0.4799999892711639404296875, 0.100000001490116119384765625).y : _891.y, _892.z ? vec3(0.62000000476837158203125, 0.4799999892711639404296875, 0.100000001490116119384765625).z : _891.z) * 0.2199999988079071044921875) * clamp(dot(_794, vec3(0.2125999927520751953125, 0.715200006961822509765625, 0.072200000286102294921875)) * 8.3333339691162109375, 0.60000002384185791015625, 1.39999997615814208984375), vec3(smoothstep(0.300000011920928955078125, 0.62000000476837158203125, ((fract(sin(dot(floor(input_worldPosition.xz * vec2(0.181818187236785888671875)), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875) * 0.550000011920928955078125) + (mix(mix(fract(sin(dot(_926, vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_926 + vec2(1.0, 0.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _936.x), mix(fract(sin(dot(_926 + vec2(0.0, 1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), fract(sin(dot(_926 + vec2(1.0), vec2(127.09999847412109375, 311.70001220703125))) * 43758.546875), _936.x), _936.y) * 0.449999988079071044921875)) + ((_100.g_terrainVariation.y - 0.5) * 0.25)) * _100.g_terrainVariation.y));
        break;
    } while(false);
    vec3 _1020 = normalize(normalize(input_normal + ((vec3(sin(_751.x), 0.0, cos(_751.z)) * 0.180000007152557373046875) * _756)));
    float _1357;
    do
    {
        if (_100.g_shadowParameters.w <= 0.0)
        {
            _1357 = 1.0;
            break;
        }
        vec4 _1127 = _100.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _1133 = _1127.xyz / vec3(_1127.w);
        vec2 _1138 = (_1133.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_1138, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_1138, vec2(0.99800002574920654296875)))) || (_1133.z < 0.0)) || (_1133.z > 1.0))
        {
            _1357 = 1.0;
            break;
        }
        float _1166 = _100.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_1020, _170.g_sunDirection), 0.0, 1.0))));
        float _1350;
        _1350 = 0.0;
        float _1355;
        SPIRV_CROSS_UNROLL
        for (int _1349 = -1; _1349 <= 1; _1350 = _1355, _1349++)
        {
            _1355 = _1350;
            float _1225;
            SPIRV_CROSS_UNROLL
            for (int _1351 = -1; _1351 <= 1; _1355 = _1225, _1351++)
            {
                vec4 _1191 = textureLod(Texture1, _1138 + (vec2(float(_1351), float(_1349)) * _100.g_shadowParameters.xy), 0.0);
                float _1192 = _1191.x;
                float _1353;
                if (_100.g_dynamicShadow.x > 0.5)
                {
                    _1353 = max(_1192, textureLod(Texture3, _1138 + (vec2(float(_1351), float(_1349)) * _100.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _1353 = _1192;
                }
                _1225 = _1355 + float(int((_1133.z + _1166) >= _1353));
            }
        }
        _1357 = mix(1.0, _1350 * 0.111111111938953399658203125, _100.g_shadowParameters.w * clamp(min(min(_1138.x, _1138.y), min(1.0 - _1138.x, 1.0 - _1138.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _1028 = _170.g_sunColor * _1357;
    vec3 _1094 = mix(_100.g_fogColorDensity.xyz, ((((_170.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_1020.y * 0.5) + 0.5, 0.0, 1.0)))) + (_1028 * clamp(dot(_1020, _170.g_sunDirection), 0.0, 1.0))) * vec4(_1347, 1.0).xyz) + ((_1028 * (_362.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_1020, normalize(normalize(_345.g_eyePosition - input_worldPosition) + _170.g_sunDirection)), 0.0, 1.0), max(8.0, _362.g_shininess)) * float(0.0 < dot(_1020, _170.g_sunDirection))))) + _362.g_emissionColor, vec3(exp((-_100.g_fogColorDensity.w) * distance(_345.g_eyePosition, input_worldPosition))));
    _entryPointOutput = vec4(_1094, 1.0);
}


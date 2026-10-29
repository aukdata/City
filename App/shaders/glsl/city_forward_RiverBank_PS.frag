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
} _84;

layout(std140) uniform PSPerFrame
{
    vec3 g_globalAmbientColor;
    vec3 g_sunColor;
    vec3 g_sunDirection;
} _155;

layout(std140) uniform PSPerView
{
    vec3 g_eyePosition;
} _330;

layout(std140) uniform PSPerMaterial
{
    vec3 g_ambientColor;
    uint g_hasTexture;
    vec4 g_diffuseColor;
    vec3 g_specularColor;
    float g_shininess;
    vec3 g_emissionColor;
} _347;

uniform sampler2D Texture0;
uniform sampler2D Texture1;
uniform sampler2D Texture3;

layout(location = 0) in vec3 input_worldPosition;
layout(location = 1) in vec2 input_uv;
layout(location = 2) in vec3 input_normal;
layout(location = 0) out vec4 _entryPointOutput;

void main()
{
    float _611 = max(1.0, input_uv.y);
    float _629 = sin((input_worldPosition.x * 0.071000002324581146240234375) + sin(input_worldPosition.z * 0.037000000476837158203125)) * sin((input_worldPosition.z * 0.0610000006854534149169921875) + (input_worldPosition.x * 0.0130000002682209014892578125));
    float _639 = 1.0 - smoothstep(_611 - 5.0, _611 + 2.0, input_uv.x + (_629 * 2.2999999523162841796875));
    if (_639 < 0.008000000379979610443115234375)
    {
        discard;
    }
    vec2 _660 = vec2((input_worldPosition.x * 0.97237002849578857421875) - (input_worldPosition.z * 0.233449995517730712890625), (input_worldPosition.x * 0.233449995517730712890625) + (input_worldPosition.z * 0.97237002849578857421875)) * vec2(0.100000001490116119384765625);
    vec4 _673 = texture(Texture0, _660);
    vec4 _679 = texture(Texture0, (vec2(-_660.y, _660.x) * 0.17299999296665191650390625) + vec2(0.310000002384185791015625, 0.670000016689300537109375));
    vec3 _730 = normalize(input_normal);
    float _1063;
    do
    {
        if (_84.g_shadowParameters.w <= 0.0)
        {
            _1063 = 1.0;
            break;
        }
        vec4 _837 = _84.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _843 = _837.xyz / vec3(_837.w);
        vec2 _848 = (_843.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_848, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_848, vec2(0.99800002574920654296875)))) || (_843.z < 0.0)) || (_843.z > 1.0))
        {
            _1063 = 1.0;
            break;
        }
        float _876 = _84.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_730, _155.g_sunDirection), 0.0, 1.0))));
        float _1056;
        _1056 = 0.0;
        float _1061;
        SPIRV_CROSS_UNROLL
        for (int _1055 = -1; _1055 <= 1; _1056 = _1061, _1055++)
        {
            _1061 = _1056;
            float _935;
            SPIRV_CROSS_UNROLL
            for (int _1057 = -1; _1057 <= 1; _1061 = _935, _1057++)
            {
                vec4 _901 = textureLod(Texture1, _848 + (vec2(float(_1057), float(_1055)) * _84.g_shadowParameters.xy), 0.0);
                float _902 = _901.x;
                float _1059;
                if (_84.g_dynamicShadow.x > 0.5)
                {
                    _1059 = max(_902, textureLod(Texture3, _848 + (vec2(float(_1057), float(_1055)) * _84.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _1059 = _902;
                }
                _935 = _1061 + float(int((_843.z + _876) >= _1059));
            }
        }
        _1063 = mix(1.0, _1056 * 0.111111111938953399658203125, _84.g_shadowParameters.w * clamp(min(min(_848.x, _848.y), min(1.0 - _848.x, 1.0 - _848.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _738 = _155.g_sunColor * _1063;
    vec3 _787 = ((((_155.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_730.y * 0.5) + 0.5, 0.0, 1.0)))) + (_738 * clamp(dot(_730, _155.g_sunDirection), 0.0, 1.0))) * vec4((mix(_673.xyz, _679.xyz, vec3(0.37999999523162841796875)) * mix(vec3(0.4000000059604644775390625, 0.37000000476837158203125, 0.310000002384185791015625), vec3(0.7200000286102294921875, 0.689999997615814208984375, 0.60000002384185791015625), vec3(smoothstep(2.0, 14.0, input_uv.x)))) * (1.0 + (_629 * 0.064999997615814208984375)), _639).xyz) + ((_738 * (_347.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_730, normalize(normalize(_330.g_eyePosition - input_worldPosition) + _155.g_sunDirection)), 0.0, 1.0), max(8.0, _347.g_shininess)) * float(0.0 < dot(_730, _155.g_sunDirection))))) + _347.g_emissionColor;
    _entryPointOutput = vec4(mix(_84.g_fogColorDensity.xyz, _787, vec3(exp((-_84.g_fogColorDensity.w) * distance(_330.g_eyePosition, input_worldPosition)))), _639);
}


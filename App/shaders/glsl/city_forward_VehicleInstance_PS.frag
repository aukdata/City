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

layout(std140) uniform PSPerMaterial
{
    vec3 g_ambientColor;
    uint g_hasTexture;
    vec4 g_diffuseColor;
    vec3 g_specularColor;
    float g_shininess;
    vec3 g_emissionColor;
} _57;

layout(std140) uniform CityParameters
{
    mat4 g_worldToShadow;
    vec4 g_shadowParameters;
    vec4 g_fogColorDensity;
    vec4 g_dynamicShadow;
    vec4 g_altitudeBands;
    vec4 g_terrainVariation;
} _126;

layout(std140) uniform PSPerFrame
{
    vec3 g_globalAmbientColor;
    vec3 g_sunColor;
    vec3 g_sunDirection;
} _193;

layout(std140) uniform PSPerView
{
    vec3 g_eyePosition;
} _360;

uniform sampler2D Texture0;
uniform sampler2D Texture1;
uniform sampler2D Texture3;

layout(location = 0) in vec3 input_worldPosition;
layout(location = 1) in vec2 input_uv;
layout(location = 2) in vec3 input_normal;
layout(location = 3) in vec4 input_paint;
layout(location = 0) out vec4 _entryPointOutput;

void main()
{
    vec4 _892;
    if (_57.g_hasTexture != 0u)
    {
        _892 = _57.g_diffuseColor * texture(Texture0, input_uv);
    }
    else
    {
        _892 = _57.g_diffuseColor;
    }
    vec4 _526 = _892 * input_paint;
    vec3 _574 = normalize(input_normal);
    float _901;
    do
    {
        if (_126.g_shadowParameters.w <= 0.0)
        {
            _901 = 1.0;
            break;
        }
        vec4 _681 = _126.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _687 = _681.xyz / vec3(_681.w);
        vec2 _692 = (_687.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_692, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_692, vec2(0.99800002574920654296875)))) || (_687.z < 0.0)) || (_687.z > 1.0))
        {
            _901 = 1.0;
            break;
        }
        float _720 = _126.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_574, _193.g_sunDirection), 0.0, 1.0))));
        float _894;
        _894 = 0.0;
        float _899;
        SPIRV_CROSS_UNROLL
        for (int _893 = -1; _893 <= 1; _894 = _899, _893++)
        {
            _899 = _894;
            float _779;
            SPIRV_CROSS_UNROLL
            for (int _895 = -1; _895 <= 1; _899 = _779, _895++)
            {
                vec4 _745 = textureLod(Texture1, _692 + (vec2(float(_895), float(_893)) * _126.g_shadowParameters.xy), 0.0);
                float _746 = _745.x;
                float _897;
                if (_126.g_dynamicShadow.x > 0.5)
                {
                    _897 = max(_746, textureLod(Texture3, _692 + (vec2(float(_895), float(_893)) * _126.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _897 = _746;
                }
                _779 = _899 + float(int((_687.z + _720) >= _897));
            }
        }
        _901 = mix(1.0, _894 * 0.111111111938953399658203125, _126.g_shadowParameters.w * clamp(min(min(_692.x, _692.y), min(1.0 - _692.x, 1.0 - _692.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _582 = _193.g_sunColor * _901;
    vec3 _648 = mix(_126.g_fogColorDensity.xyz, ((((_193.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_574.y * 0.5) + 0.5, 0.0, 1.0)))) + (_582 * clamp(dot(_574, _193.g_sunDirection), 0.0, 1.0))) * _526.xyz) + ((_582 * (_57.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_574, normalize(normalize(_360.g_eyePosition - input_worldPosition) + _193.g_sunDirection)), 0.0, 1.0), max(8.0, _57.g_shininess)) * float(0.0 < dot(_574, _193.g_sunDirection))))) + _57.g_emissionColor, vec3(exp((-_126.g_fogColorDensity.w) * distance(_360.g_eyePosition, input_worldPosition))));
    _entryPointOutput = vec4(_648, _526.w);
}


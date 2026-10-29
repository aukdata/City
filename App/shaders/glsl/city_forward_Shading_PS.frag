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
uniform sampler2D Texture1;
uniform sampler2D Texture3;

layout(location = 0) in vec3 input_worldPosition;
layout(location = 1) in vec2 input_uv;
layout(location = 2) in vec3 input_normal;
layout(location = 0) out vec4 _entryPointOutput;

void main()
{
    vec4 _847;
    if (_55.g_hasTexture != 0u)
    {
        _847 = _55.g_diffuseColor * texture(Texture0, input_uv);
    }
    else
    {
        _847 = _55.g_diffuseColor;
    }
    vec3 _540 = normalize(input_normal);
    float _856;
    do
    {
        if (_124.g_shadowParameters.w <= 0.0)
        {
            _856 = 1.0;
            break;
        }
        vec4 _647 = _124.g_worldToShadow * vec4(input_worldPosition, 1.0);
        vec3 _653 = _647.xyz / vec3(_647.w);
        vec2 _658 = (_653.xy * vec2(0.5, -0.5)) + vec2(0.5);
        if (((any(lessThan(_658, vec2(0.00200000009499490261077880859375))) || any(greaterThan(_658, vec2(0.99800002574920654296875)))) || (_653.z < 0.0)) || (_653.z > 1.0))
        {
            _856 = 1.0;
            break;
        }
        float _686 = _124.g_shadowParameters.z * (1.0 + (3.0 * (1.0 - clamp(dot(_540, _191.g_sunDirection), 0.0, 1.0))));
        float _849;
        _849 = 0.0;
        float _854;
        SPIRV_CROSS_UNROLL
        for (int _848 = -1; _848 <= 1; _849 = _854, _848++)
        {
            _854 = _849;
            float _745;
            SPIRV_CROSS_UNROLL
            for (int _850 = -1; _850 <= 1; _854 = _745, _850++)
            {
                vec4 _711 = textureLod(Texture1, _658 + (vec2(float(_850), float(_848)) * _124.g_shadowParameters.xy), 0.0);
                float _712 = _711.x;
                float _852;
                if (_124.g_dynamicShadow.x > 0.5)
                {
                    _852 = max(_712, textureLod(Texture3, _658 + (vec2(float(_850), float(_848)) * _124.g_shadowParameters.xy), 0.0).x);
                }
                else
                {
                    _852 = _712;
                }
                _745 = _854 + float(int((_653.z + _686) >= _852));
            }
        }
        _856 = mix(1.0, _849 * 0.111111111938953399658203125, _124.g_shadowParameters.w * clamp(min(min(_658.x, _658.y), min(1.0 - _658.x, 1.0 - _658.y)) * 24.0, 0.0, 1.0));
        break;
    } while(false);
    vec3 _548 = _191.g_sunColor * _856;
    vec3 _614 = mix(_124.g_fogColorDensity.xyz, ((((_191.g_globalAmbientColor * mix(vec3(0.579999983310699462890625, 0.540000021457672119140625, 0.4799999892711639404296875), vec3(0.87999999523162841796875, 0.949999988079071044921875, 1.059999942779541015625), vec3(clamp((_540.y * 0.5) + 0.5, 0.0, 1.0)))) + (_548 * clamp(dot(_540, _191.g_sunDirection), 0.0, 1.0))) * _847.xyz) + ((_548 * (_55.g_specularColor * 0.3499999940395355224609375)) * (pow(clamp(dot(_540, normalize(normalize(_358.g_eyePosition - input_worldPosition) + _191.g_sunDirection)), 0.0, 1.0), max(8.0, _55.g_shininess)) * float(0.0 < dot(_540, _191.g_sunDirection))))) + _55.g_emissionColor, vec3(exp((-_124.g_fogColorDensity.w) * distance(_358.g_eyePosition, input_worldPosition))));
    _entryPointOutput = vec4(_614, _847.w);
}


// Generated from the matching HLSL entry point by chore/generate_linux_shaders.py.
// Fixed tree meshes are shared across chunks and LOD changes.
#version 410

struct TreeTransform
{
    vec4 positionWidth;
    vec4 heightRotation;
};

layout(std140, row_major) uniform VSPerView
{
    mat4 g_worldToProjected;
} _93;

layout(std140) uniform NearTreeInstances
{
    TreeTransform g_nearTrees[64];
} _145;

layout(location = 0) in vec4 input_position;
layout(location = 1) in vec3 input_normal;
layout(location = 2) in vec2 input_uv;
layout(location = 0) out vec3 _entryPointOutput_worldPosition;
layout(location = 1) out vec2 _entryPointOutput_uv;
layout(location = 2) out vec3 _entryPointOutput_normal;

void main()
{
    uint _205 = uint(input_uv.x);
    vec3 _229 = vec3(_145.g_nearTrees[_205].positionWidth.w, _145.g_nearTrees[_205].heightRotation.x, _145.g_nearTrees[_205].positionWidth.w);
    vec3 _238 = input_position.xyz * _229;
    vec3 _264 = vec3((_238.x * _145.g_nearTrees[_205].heightRotation.y) + (_238.z * _145.g_nearTrees[_205].heightRotation.z), _238.y, ((-_238.x) * _145.g_nearTrees[_205].heightRotation.z) + (_238.z * _145.g_nearTrees[_205].heightRotation.y)) + _145.g_nearTrees[_205].positionWidth.xyz;
    vec3 _269 = normalize(input_normal * _229);
    gl_Position = _93.g_worldToProjected * vec4(_264, 1.0);
    _entryPointOutput_worldPosition = _264;
    _entryPointOutput_uv = vec2(0.0);
    _entryPointOutput_normal = normalize(vec3((_269.x * _145.g_nearTrees[_205].heightRotation.y) + (_269.z * _145.g_nearTrees[_205].heightRotation.z), _269.y, ((-_269.x) * _145.g_nearTrees[_205].heightRotation.z) + (_269.z * _145.g_nearTrees[_205].heightRotation.y)));
}


// Generated from the matching HLSL entry point by chore/generate_linux_shaders.py.
// Far vehicle geometry is shared; only the world matrices change each frame.
#version 410

layout(std140) uniform VehicleInstances
{
    mat4 g_instanceWorld[256];
    vec4 g_instanceColors[256];
} _26;

layout(std140, row_major) uniform VSPerView
{
    mat4 g_worldToProjected;
} _48;

layout(location = 0) in vec4 input_position;
layout(location = 1) in vec3 input_normal;
layout(location = 2) in vec2 input_uv;
layout(location = 0) out vec3 _entryPointOutput_worldPosition;
layout(location = 1) out vec2 _entryPointOutput_uv;
layout(location = 2) out vec3 _entryPointOutput_normal;
layout(location = 3) out vec4 _entryPointOutput_paint;

void main()
{
    uint _130 = uint(input_uv.x);
    vec4 _136 = _26.g_instanceWorld[_130] * input_position;
    gl_Position = _48.g_worldToProjected * _136;
    _entryPointOutput_worldPosition = _136.xyz;
    _entryPointOutput_uv = vec2(0.0);
    _entryPointOutput_normal = mat3(_26.g_instanceWorld[_130][0].xyz, _26.g_instanceWorld[_130][1].xyz, _26.g_instanceWorld[_130][2].xyz) * input_normal;
    _entryPointOutput_paint = _26.g_instanceColors[uint(input_uv.x)];
}


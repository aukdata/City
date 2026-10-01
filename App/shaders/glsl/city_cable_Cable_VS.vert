// Generated from the matching HLSL entry point by chore/generate_linux_shaders.py.
// Screen-space coverage for real 28mm cables. Thin distant wires fade smoothly.
#version 410

layout(std140, row_major) uniform VSPerObject
{
    mat4 g_localToWorld;
} _22;

layout(std140, row_major) uniform VSPerView
{
    mat4 g_worldToProjected;
} _34;

layout(std140) uniform CableView
{
    vec4 g_viewport;
} _73;

layout(location = 0) in vec4 input_position;
layout(location = 1) in vec3 input_normal;
layout(location = 2) in vec2 input_uv;
layout(location = 0) out vec2 _entryPointOutput_uv;
layout(location = 1) out float _entryPointOutput_coverage;

void main()
{
    vec4 _206 = _34.g_worldToProjected * (_22.g_localToWorld * input_position);
    vec4 _218 = _34.g_worldToProjected * (_22.g_localToWorld * vec4(input_normal, 0.0));
    vec2 _233 = ((_218.xy * _206.w) - (_206.xy * _218.w)) * _73.g_viewport.xy;
    vec2 _239 = _233 / vec2(max(length(_233), 9.9999997473787516355514526367188e-05));
    float _255 = ((input_uv.y * length(vec3(_34.g_worldToProjected[0].x, _34.g_worldToProjected[1].x, _34.g_worldToProjected[2].x))) * _73.g_viewport.x) * 0.5;
    float _261 = max(0.75 * abs(_206.w), _255);
    vec2 _280 = _206.xy + ((((vec2(-_239.y, _239.x) * input_uv.x) * _261) * 2.0) * _73.g_viewport.zw);
    vec4 _345 = _206;
    _345.x = _280.x;
    _345.y = _280.y;
    gl_Position = _345;
    _entryPointOutput_uv = vec2(input_uv.x, 0.0);
    _entryPointOutput_coverage = clamp(_255 / max(_261, 9.9999997473787516355514526367188e-05), 0.0, 1.0);
}


// Generated from the matching HLSL entry point by chore/generate_linux_shaders.py.
// Screen-space coverage for real 28mm cables. Thin distant wires fade smoothly.
#version 410

layout(location = 0) in vec2 input_uv;
layout(location = 1) in float input_coverage;
layout(location = 0) out vec4 _entryPointOutput;

void main()
{
    _entryPointOutput = vec4(0.014999999664723873138427734375, 0.017999999225139617919921875, 0.0199999995529651641845703125, input_coverage * (1.0 - smoothstep(0.449999988079071044921875, 1.0, abs(input_uv.x))));
}


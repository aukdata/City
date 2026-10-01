// Generated from the matching HLSL entry point by chore/generate_linux_shaders.py.
//-----------------------------------------------
//
//	選択オブジェクトのアウトライン生成 2D PS
//	入力: 選択オブジェクトをソリッド白で描画したマスクテクスチャ
//	出力: マスクの輪郭のみをカラー付きで返し、内側は透明
//	方式: Dilate - Self（中央が 0 で周辺に 1 があれば縁）
//
//-----------------------------------------------

#version 410

layout(std140) uniform OutlineParams
{
    vec4 g_texelSize;
    vec4 g_outlineColor;
    vec4 g_outlineScale;
} _48;

uniform sampler2D Texture0;

layout(location = 1) in vec2 input_uv;
layout(location = 0) out vec4 _entryPointOutput;

void main()
{
    vec4 _414;
    do
    {
        if (texture(Texture0, input_uv).w > 0.5)
        {
            _414 = vec4(0.0);
            break;
        }
        vec2 _258 = _48.g_texelSize.xy * max(_48.g_outlineScale.x, 1.0);
        _414 = vec4(_48.g_outlineColor.xyz, _48.g_outlineColor.w * clamp(((((((texture(Texture0, input_uv + vec2(-_258.x, -_258.y)).w + texture(Texture0, input_uv + vec2(0.0, -_258.y)).w) + texture(Texture0, input_uv + vec2(_258.x, -_258.y)).w) + texture(Texture0, input_uv + vec2(-_258.x, 0.0)).w) + texture(Texture0, input_uv + vec2(_258.x, 0.0)).w) + texture(Texture0, input_uv + vec2(-_258.x, _258.y)).w) + texture(Texture0, input_uv + vec2(0.0, _258.y)).w) + texture(Texture0, input_uv + _258).w, 0.0, 1.0));
        break;
    } while(false);
    _entryPointOutput = _414;
}


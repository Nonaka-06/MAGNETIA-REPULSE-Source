#include "inc_KdPostProcessShader.hlsli"

// 白飛びの抑制(ハイライトの肩)
//
// シーンはHDR(1.0を超える色)で描かれ、そのまま8bitのRTへ書くと各色が1.0で切られる。
// シアン(0.3, 0.85, 1.0)の発光を3倍に重ねると(0.9, 1, 1)となり、ほぼ白になる。
// 一番明るい成分が肩(g_knee)を超えた分だけを滑らかに1.0へ寄せ、色の比は保つ。
// 肩以下の画素は変えない。肩〜1.0の画素はわずかに暗くなる(白へ寄せる割合が小さいほど強く暗くなる)

Texture2D g_inputTex : register(t0);
SamplerState g_ss : register(s0);

cbuffer cbHighlightCompress : register(b0)
{
	float g_knee;		// 圧縮を始める明るさ(0〜1)。下げるほど早くから抑える
	float g_whiteMix;	// 従来の切り捨て(白へ寄る)を混ぜる割合。0で色を完全に保つ
	float2 g_blank;
};

float4 main(VSOutput In) : SV_Target0
{
	float4 texColor = g_inputTex.Sample(g_ss, In.UV);

	const float peak = max(texColor.r, max(texColor.g, texColor.b));

	if (peak <= g_knee)
	{
		return texColor;
	}

	// 肩から先を指数で1.0へ近づける。肩の位置で傾きが1になり、折れ目が出ない
	const float range = max(1.0f - g_knee, 1e-4f);
	const float mapped = g_knee + range * (1.0f - exp(-(peak - g_knee) / range));

	const float3 hueKept = texColor.rgb * (mapped / peak);
	texColor.rgb = lerp(hueKept, saturate(texColor.rgb), g_whiteMix);

	return texColor;
}

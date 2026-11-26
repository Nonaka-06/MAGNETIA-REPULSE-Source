// このファイルはBaseFramework(提供テンプレート)には存在しない、独自追加ファイルです。

#include "../SceneTransition/inc_TransitionCommon.hlsli"

//=============================================================
// Transition_CinematicGrading_PS.hlsl
// 画面全体へ掛けるカラーグレーディング。
// ボス戦ムービー中の強い演出と、通常プレイ中の弱い常時適用の
// 両方で使う(強度と各パラメータで効き方を変える)
//
// 演出:
//   ・彩度を底上げしてビビッドな発色にする
//   ・画面中心からの距離に応じて周辺を減光するビネットを掛ける
//   ・スプリットトーン(暗部を寒色へ、明部を暖色へ)で夜の空気感を作る
//   ・フィルムグレイン(暗部のバンディング隠しも兼ねる)
//   ・画面端だけごく弱い色収差
//   ・Intensityで通常描画とのブレンド比率を制御し、フェードイン/アウトできる
//=============================================================

// スロット 0 : 元シーンテクスチャ
Texture2D    g_SceneTex : register(t0);
SamplerState g_Sampler  : register(s0);

//--------------------------------------------------------------
// 定数バッファ(スロット 1)
//--------------------------------------------------------------
cbuffer cbCinematicGrading : register(b1)
{
	float g_Intensity;        // 適用強度(0=無効 1=全開)
	float g_SaturationBoost;  // 彩度倍率(1.0で変化なし)
	float g_VignetteStrength; // ビネットの強さ(0〜1)
	float g_GrainStrength;    // フィルムグレインの強さ(0で無効)

	float3 g_ShadowTint;      // 暗部へ寄せる色(スプリットトーン)
	float g_ChromaStrength;   // 色収差の強さ(0で無効。画面端ほど強く出る)

	float3 g_HighlightTint;   // 明部へ寄せる色(スプリットトーン)
	float g_Time;             // グレインを動かすための経過時間(秒)

	float2 g_ChromaCenter;    // 色収差の中心(UV)。ここから放射状にRとBがずれる
	float g_ChromaEdgePower;  // 中心からの距離に対する立ち上がり。1未満で中心寄りにも出る
	float g_ChromaPad;        // 16バイト境界合わせ
	float2 g_VignetteCenter;  // ビネットの中心(UV)。注視させたい被写体へ寄せると、その周りだけが明るく残る
	float2 g_VignettePad;     // 16バイト境界合わせ
};

//--------------------------------------------------------------
// メイン
//--------------------------------------------------------------
float4 main(VSOutput input) : SV_TARGET
{
	float2 uv = input.UV;
	float2 centered = uv - 0.5f;

	//----------------------------------------------------------
	// 色収差(中心から離れるほど強く、RとBを逆方向へずらす)。
	// 中心はg_ChromaCenterで動かせる。画面中央のままならレンズ収差、
	// 咆哮の口元等へ寄せると「その点から放射状に色が裂ける」歪みになる
	//----------------------------------------------------------
	const float4 sceneCol = SampleChromaticAberration(
		g_SceneTex, g_Sampler, uv, g_ChromaCenter, g_ChromaStrength, g_ChromaEdgePower);

	//----------------------------------------------------------
	// 彩度ブースト(輝度を軸にlerp)
	//----------------------------------------------------------
	float  lum = TransitionLuminance709(sceneCol.rgb);
	float3 saturated = lerp(float3(lum, lum, lum), sceneCol.rgb, g_SaturationBoost);

	//----------------------------------------------------------
	// スプリットトーン(暗部を寒色へ、明部を暖色へ)
	// 夜の廃墟は色が乏しく灰色一色になりやすいため、
	// 明暗で色の方向を分けて空気感を作る
	//----------------------------------------------------------
	float  toneLum = TransitionLuminance709(saturated);
	float3 tint = lerp(g_ShadowTint, g_HighlightTint, saturate(toneLum));

	// 乗算ではなく中心1.0からのズレとして加算し、明るさを保ったまま色味だけ振る
	float3 toned = saturate(saturated * (tint * 2.0f));

	//----------------------------------------------------------
	// ビネット(中心からの距離2乗で減光。中心は既定で画面中央)
	//----------------------------------------------------------
	float2 vignetteOffset = uv - g_VignetteCenter;
	float  vignette = 1.0f - saturate(dot(vignetteOffset, vignetteOffset) * 2.0f) * g_VignetteStrength;

	float3 graded = toned * vignette;

	//----------------------------------------------------------
	// フィルムグレイン
	// 暗部ほど強く乗せる。明部に乗せると汚れて見えるだけだが、
	// 暗部ではバンディング(階調の段差)を隠す実利がある
	//----------------------------------------------------------
	if (g_GrainStrength > 0.0f)
	{
		float noise = TransitionHash2(uv * (1.0f + g_Time));

		float  gradedLum = TransitionLuminance709(graded);
		float  darkWeight = 1.0f - saturate(gradedLum * 1.5f);

		graded += (noise - 0.5f) * g_GrainStrength * darkWeight;
	}

	float3 result = lerp(sceneCol.rgb, saturate(graded), g_Intensity);

	return float4(result, sceneCol.a);
}

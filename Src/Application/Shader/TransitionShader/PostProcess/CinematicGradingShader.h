// このファイルはBaseFramework(提供テンプレート)には存在しない、独自追加ファイルです。

#pragma once

#include "Application/Shader/TransitionShader/SceneTransition/BaseTransitionShader.h"

//=============================================================
// ボス戦ムービー中に常時適用するシネマティックグレーディング
// (彩度ブースト+ビネット)。BaseTransitionShaderの土台を借りるが、
// 一発トランジションではなく、CinematicGradingがフェード管理する
// 継続的なポストエフェクトとして毎フレームApply()される想定
//
// HLSL: Transition_CinematicGrading_PS.hlsl
//=============================================================

//=============================================================
// 定数バッファ構造体(HLSLのcbCinematicGradingと対応)
// 16バイトアライメント厳守
//=============================================================
struct CbCinematicGrading
{
	float			Intensity = 0.0f;
	float			SaturationBoost = 1.0f;
	float			VignetteStrength = 0.0f;
	float			GrainStrength = 0.0f;

	// スプリットトーン。0.5 が無彩色で、そこからのズレが色味になる
	Math::Vector3	ShadowTint = { 0.5f, 0.5f, 0.5f };
	float			ChromaStrength = 0.0f;

	Math::Vector3	HighlightTint = { 0.5f, 0.5f, 0.5f };
	float			Time = 0.0f;

	Math::Vector2	ChromaCenter = { 0.5f, 0.5f };
	float			ChromaEdgePower = 1.0f;
	float			ChromaPad = 0.0f;
	Math::Vector2	VignetteCenter = { 0.5f, 0.5f };
	Math::Vector2	VignettePad = { 0.0f, 0.0f };
};

class CinematicGradingShader final : public BaseTransitionShader<CbCinematicGrading>
{
public:

	bool Init();

	//----------------------------------------------------------
	// 1フレームぶんの見た目
	//----------------------------------------------------------
	struct Params
	{
		// 適用強度(0=無効 1=全開)
		float			intensity = 0.0f;

		// 彩度倍率(1.0で変化なし)
		float			saturationBoost = 1.8f;

		// ビネットの強さ(0〜1)
		float			vignetteStrength = 0.55f;

		// フィルムグレインの強さ(0で無効)
		float			grainStrength = 0.0f;

		// 色収差の強さ(0で無効。中心から離れるほど強く出る)
		float			chromaStrength = 0.0f;

		// スプリットトーン。0.5が無彩色で、そこからのズレが色味になる
		Math::Vector3	shadowTint = { 0.5f, 0.5f, 0.5f };
		Math::Vector3	highlightTint = { 0.5f, 0.5f, 0.5f };

		// グレインを動かすための経過時間[秒]
		float			time = 0.0f;

		// 色収差の中心(UV)。既定の(0.5, 0.5)でレンズ収差、画面上の
		// 一点へ寄せるとそこから放射状に裂ける歪みになる
		Math::Vector2	chromaCenter = { 0.5f, 0.5f };

		// 中心からの距離に対する立ち上がり。1未満で中心寄りにも出る
		float			chromaEdgePower = 1.0f;
		// ビネットの中心(UV)。既定の(0.5, 0.5)で画面中央、被写体へ寄せるとその周りが明るく残る
		Math::Vector2	vignetteCenter = { 0.5f, 0.5f };
	};

	//----------------------------------------------------------
	// 適用
	// spSceneTex : 元シーンのテクスチャ
	//----------------------------------------------------------
	void Apply(const std::shared_ptr<KdTexture>& a_spSceneTex, const Params& a_params);
};

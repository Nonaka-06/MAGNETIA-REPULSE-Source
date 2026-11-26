// このファイルはBaseFramework(提供テンプレート)には存在しない、独自追加ファイルです。

#include "Application/Shader/TransitionShader/PostProcess/CinematicGradingShader.h"

// コンパイル済みシェーダーバイトコード
#include "Application/Shader/TransitionShader/PostProcess/Transition_CinematicGrading_PS.shaderInc"

bool CinematicGradingShader::Init()
{
	return CreateResources(
		g_PS_Transition_CinematicGrading,
		sizeof(g_PS_Transition_CinematicGrading),
		"CinematicGradingShader");
}

void CinematicGradingShader::Apply(
	const std::shared_ptr<KdTexture>& a_spSceneTex,
	const Params& a_params
)
{
	CbCinematicGrading& cb = WorkCb();

	cb.Intensity = a_params.intensity;
	cb.SaturationBoost = a_params.saturationBoost;
	cb.VignetteStrength = a_params.vignetteStrength;
	cb.GrainStrength = a_params.grainStrength;
	cb.ChromaStrength = a_params.chromaStrength;
	cb.ShadowTint = a_params.shadowTint;
	cb.HighlightTint = a_params.highlightTint;
	cb.Time = a_params.time;
	cb.ChromaCenter = a_params.chromaCenter;
	cb.ChromaEdgePower = a_params.chromaEdgePower;
	cb.VignetteCenter = a_params.vignetteCenter;

	// t0: シーン
	DrawFullscreen({ a_spSceneTex });
}

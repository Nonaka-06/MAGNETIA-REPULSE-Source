// このファイルはBaseFramework(提供テンプレート)には存在しない、独自追加ファイルです。

//=============================================================
// 2D板ポリGPUパーティクル用シェーダー実装
//=============================================================

#include "Application/Shader/ParticleShader/Particle/ParticleShader.h"
#include "Application/Shader/ParticleShader/Particle/ParticleShaderCommon.h"
#include "Application/Shader/KdComputeShader.h"

// ビルド時に fxc がコンパイルしたバイナリをインクルードする
#include "Application/Shader/ParticleShader/Particle/Particle_VS.shaderInc"
#include "Application/Shader/ParticleShader/Particle/Particle_PS.shaderInc"

KdBlendState Particle::ToKdBlendState(BlendMode a_mode)
{
	return (a_mode == BlendMode::Add) ? KdBlendState::Add : KdBlendState::Alpha;
}

//=============================================================
// Init
// シェーダー・入力レイアウト・定数バッファを初期化する
//=============================================================
bool ParticleShader::Init()
{
	Release();

	auto* pDev = KdDirect3D::Instance().WorkDev();

	//----------------------------------------------------------
	// 頂点シェーダー
	// SV_VertexID のみ使用するため InputLayout は不要
	//----------------------------------------------------------
	if (FAILED(pDev->CreateVertexShader(g_VS_Particle, sizeof(g_VS_Particle), nullptr, m_pVs.GetAddressOf())))
	{
		assert(0 && "ParticleShader: 頂点シェーダー作成失敗");
		Release();
		return false;
	}

	//----------------------------------------------------------
	// ピクセルシェーダー
	//----------------------------------------------------------
	if (FAILED(pDev->CreatePixelShader(g_PS_Particle, sizeof(g_PS_Particle), nullptr, m_pPs.GetAddressOf())))
	{
		assert(0 && "ParticleShader: ピクセルシェーダー作成失敗");
		Release();
		return false;
	}

	//----------------------------------------------------------
	// 定数バッファ
	//----------------------------------------------------------
	if (!m_cbEmitter.Create())
	{
		assert(0 && "ParticleShader: 定数バッファ作成失敗");
		Release();
		return false;
	}

	return true;
}

void ParticleShader::Release()
{
	m_pVs.Reset();
	m_pPs.Reset();
	m_pSb.Reset();
	m_pSrv.Reset();

	m_sbCapacity = 0;
	m_sbWriteOffset = 0;
	m_lastUploadStartIdx = 0;
	m_isDiscardNeeded = true;

	m_cbEmitter.Release();
}

//=============================================================
// CreateStructuredBuf
// 指定要素数の DYNAMIC StructuredBuffer と SRV を作成する
//=============================================================
bool ParticleShader::CreateStructuredBuf(UINT a_elementSize, UINT a_elementCnt)
{
	m_pSb.Reset();
	m_pSrv.Reset();

	if (!KdComputeShader::CreateDynamicStructuredSrv(a_elementSize, a_elementCnt, m_pSb.GetAddressOf(), m_pSrv.GetAddressOf()))
	{
		return false;
	}

	m_sbCapacity = a_elementCnt;

	return true;
}

//=============================================================
// UploadParticles
// パーティクル配列を StructuredBuffer に転送する
// 必要に応じてバッファを拡張する（現在容量の2倍で確保）
//=============================================================
bool ParticleShader::UploadParticles(const std::vector<Particle::ParticleData>& a_particles)
{
	const UINT count = static_cast<UINT>(a_particles.size());

	if (count == 0)
	{
		return true;
	}

	// 1フレーム分をまとめて置ける大きさで1度だけ確保する。
	// 1エフェクトがこれを超える場合だけ広げる
	const UINT required = std::max(RING_CAPACITY, count);

	if (m_sbCapacity < required)
	{
		if (!CreateStructuredBuf(sizeof(Particle::ParticleData), required))
		{
			return false;
		}

		m_sbWriteOffset = 0;
		m_isDiscardNeeded = true;
	}

	// 末尾に入り切らない場合は先頭へ戻る。
	// 巻き戻すときは、まだGPUが読んでいる可能性のある古い内容を
	// 上書きしないよう、あらためて捨ててから書く
	if (m_sbWriteOffset + count > m_sbCapacity)
	{
		m_sbWriteOffset = 0;
		m_isDiscardNeeded = true;
	}

	// Map/Unmap で転送（DYNAMIC バッファ）
	auto* pCtx = KdDirect3D::Instance().WorkDevContext();

	D3D11_MAPPED_SUBRESOURCE mapped = {};

	// 捨てるのはフレームの最初(と巻き戻し時)だけ。
	// 以降は「既に書いた範囲には触らない」約束で追記する
	const D3D11_MAP mapType =
		m_isDiscardNeeded ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE;

	if (FAILED(pCtx->Map(m_pSb.Get(), 0, mapType, 0, &mapped)))
	{
		return false;
	}

	const UINT copySize = sizeof(Particle::ParticleData) * count;

	Particle::ParticleData* pDst =
		static_cast<Particle::ParticleData*>(mapped.pData) + m_sbWriteOffset;

	memcpy_s(pDst, copySize, a_particles.data(), copySize);

	pCtx->Unmap(m_pSb.Get(), 0);

	// 頂点シェーダーはここを起点に読む(WriteEmitterCb()が渡す)
	m_lastUploadStartIdx = m_sbWriteOffset;

	m_sbWriteOffset += count;
	m_isDiscardNeeded = false;

	return true;
}

//=============================================================
// BeginFrame
// フレームの先頭で1回呼ぶ。次のUploadParticles()で1度だけ
// バッファを捨て、そのフレームはそこから順に追記していく
//=============================================================
void ParticleShader::BeginFrame()
{
	m_sbWriteOffset = 0;
	m_lastUploadStartIdx = 0;
	m_isDiscardNeeded = true;
	m_frameDrawCallCnt = 0;
}

//=============================================================
// Begin
// パイプラインにシェーダーをセットし、ステートを変更する
//=============================================================
void ParticleShader::Begin(Particle::BlendMode a_blendMode, ID3D11ShaderResourceView* a_pOverrideSrv)
{
	// SRV・CB は毎フレーム無条件でバインドする
	// （UploadParticles のたびにバッファ内容が変わるため）
	// pOverrideSrv が指定された場合は内部バッファではなくそちらをバインドする
	// 外部のバッファを使う経路(GPUプール)は、その先頭から読ませる
	if (a_pOverrideSrv) { m_lastUploadStartIdx = 0; }

	ID3D11ShaderResourceView* pSrvToBind = a_pOverrideSrv ? a_pOverrideSrv : m_pSrv.Get();

	ParticleDrawState::BindEmitterStage(m_pVs.Get(), m_pPs.Get(), pSrvToBind, m_cbEmitter.GetAddress());

	ParticleDrawState::Begin(Particle::ToKdBlendState(a_blendMode));
}

//=============================================================
// End
// パイプラインを元に戻す
//=============================================================
void ParticleShader::End()
{
	ParticleDrawState::End();
}

//=============================================================
// SetTex
// PS t0 にテクスチャをバインドする
// pTex が nullptr の場合は 1x1 白テクスチャで代替し
// colorStart / colorEnd の色がそのまま出力される
//=============================================================
void ParticleShader::SetTex(const KdTexture* a_pTex)
{
	// テクスチャが未設定なら白テクスチャで代替する
	ParticleDrawState::BindTexOrWhite(Particle::TEX_SLOT_COLOR, a_pTex);
}

//=============================================================
// SetNoiseTex
// ディゾルブ用ノイズテクスチャを PS t1 にセットする
//=============================================================
void ParticleShader::SetNoiseTex(const KdTexture* a_pTex)
{
	ParticleDrawState::BindTexOrWhite(1, a_pTex);
}

namespace
{
	// サイズ/アルファ/回転角カーブ。float×16サンプルをfloat4×4本へ詰め替える
	// (HLSLのcbuffer配列はスカラーだと要素毎に16バイトへパディングされるため)
	void WriteCurveLut(const std::array<float, Particle::CURVE_LUT_SAMPLES>* a_pLut, int& a_dstUseCurve, Math::Vector4* a_dstLut)
	{
		a_dstUseCurve = a_pLut ? 1 : 0;

		if (a_dstUseCurve)
		{
			float* pDst = reinterpret_cast<float*>(a_dstLut);

			for (int i = 0; i < Particle::CURVE_LUT_SAMPLES; ++i)
			{
				pDst[i] = (*a_pLut)[static_cast<size_t>(i)];
			}
		}
	}
}
void ParticleShader::WriteEmitterCb(const EmitterDrawDesc& a_desc)
{
	Particle::cbEmitter& cb = m_cbEmitter.Work();

	cb.mView = a_desc.viewMat;
	cb.aliveCnt = a_desc.aliveCnt;
	cb.renderMode = static_cast<int>(a_desc.renderMode);
	cb.stretchLenScale = a_desc.stretchLenScale;
	cb.stretchWorldAligned = a_desc.isStretchWorldAligned ? 1 : 0;
	cb.groundPlaneAligned = a_desc.isGroundPlaneAligned ? 1 : 0;
	cb.fogMode = static_cast<int>(a_desc.fogMode);
	cb.lightResponse = a_desc.lightResponse;
	cb.softDist = a_desc.softDist;
	cb.nearFadeDist = a_desc.nearFadeDist;
	cb.isSceneDepthValid = a_desc.isSceneDepthValid ? 1 : 0;

	cb.dissolveEnabled = a_desc.dissolve.isEnabled ? 1 : 0;
	cb.dissolveEdgeWidth = a_desc.dissolve.edgeWidth;
	cb.dissolveEdgeIntensity = a_desc.dissolve.edgeIntensity;
	cb.dissolveNoiseScale = a_desc.dissolve.noiseScale;
	cb.dissolveEdgeColor = a_desc.dissolve.edgeColor;

	cb.glitchEnabled = a_desc.glitch.isEnabled ? 1 : 0;
	cb.glitchBlockScale = a_desc.glitch.blockScale;
	cb.glitchIntensity = a_desc.glitch.intensity;
	cb.glitchSpeed = a_desc.glitch.speed;
	cb.glitchColorSplit = a_desc.glitch.colorSplit;
	cb.glitchTime = a_desc.glitch.time;

	WriteCurveLut(a_desc.pSizeCurveLut, cb.sizeUseCurve, cb.sizeCurveLut);
	WriteCurveLut(a_desc.pAlphaCurveLut, cb.alphaUseCurve, cb.alphaCurveLut);
	WriteCurveLut(a_desc.pRotCurveLut, cb.rotUseCurve, cb.rotCurveLut);

	// テクスチャのアスペクト比（幅 / 高さ）を計算して渡す
	// 板ポリの X 方向オフセットにこの値を掛けることで
	// テクスチャが引き延ばされずに表示される
	if (a_desc.pTex && a_desc.pTex->GetHeight() > 0)
	{
		cb.texAspect = static_cast<float>(a_desc.pTex->GetWidth())
			/ static_cast<float>(a_desc.pTex->GetHeight());
	}
	else
	{
		cb.texAspect = 1.0f;
	}

	// 直前のUploadParticles()が書いた位置を頂点シェーダーへ渡す。
	// 共有バッファへ1フレーム分を順に積むため、起点はエミッターごとに違う
	cb.particleStartIdx = static_cast<int>(m_lastUploadStartIdx);

	m_cbEmitter.Write();

	KdShaderManager::Instance().SetVSConstantBuffer(
		Particle::CB_SLOT_EMITTER,
		m_cbEmitter.GetAddress());
}

//=============================================================
// DrawParticles
// 頂点バッファなし・SV_VertexID で6頂点×count を描画する
//=============================================================
void ParticleShader::DrawParticles(int a_count) const
{
	if (a_count <= 0)
	{
		return;
	}

	auto* pCtx = KdDirect3D::Instance().WorkDevContext();

	// SV_VertexID のみ使用するため InputLayout は nullptr で確定させる
	pCtx->IASetInputLayout(nullptr);
	pCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// 6頂点 × count
	pCtx->Draw(static_cast<UINT>(a_count) * 6, 0);

	++m_frameDrawCallCnt;
}

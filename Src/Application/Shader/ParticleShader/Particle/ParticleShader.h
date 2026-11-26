// このファイルはBaseFramework(提供テンプレート)には存在しない、独自追加ファイルです。

#pragma once

//=============================================================
// 2D板ポリGPUパーティクル用シェーダークラス
//
// ・StructuredBuffer に ParticleData 配列を転送して
//   VSSetShaderResources でバインドする
// ・Draw で板ポリ4頂点 × パーティクル数を描画する
// ・ビルボード行列はカメラ情報から VS 内で計算する
// ・テクスチャのアスペクト比を cbEmitter 経由で VS に渡し
//   板ポリの引き延ばしを防ぐ
//=============================================================

#include "Application/Effect/Particle/ParticleConst.h"

class ParticleShader
{
public:

	//----------------------------------------------------------
	// 公開インターフェース
	//----------------------------------------------------------

	bool Init();
	void Release();

	// StructuredBuffer を GPU にアップロードし SRV を更新する
	// particles … 転送するパーティクル配列
	//----------------------------------------------------------
	// フレームの先頭で1回呼ぶ。リングバッファの書き込み位置を戻す
	//----------------------------------------------------------
	void BeginFrame();

	//----------------------------------------------------------
	// パーティクル配列を共有バッファへ積む。
	//
	// 【重要】1フレームに何十回も呼ばれる。毎回 MAP_WRITE_DISCARD でバッファ全体を
	// 捨てると、容量が大きいときに1回あたり約165マイクロ秒かかりフレームを圧迫する。
	// そのためdiscardは1フレームに1回だけにし、以降は末尾へ追記していく
	//----------------------------------------------------------
	bool UploadParticles(const std::vector<Particle::ParticleData>& a_particles);

	// シェーダーパイプラインをセット
	// blendMode      … ブレンドモード
	// pOverrideSrv   … 指定した場合、UploadParticles() の内部バッファではなく
	//                   このSRVをバインドする(GPUパーティクル用)
	void Begin(Particle::BlendMode a_blendMode, ID3D11ShaderResourceView* a_pOverrideSrv = nullptr);

	// シェーダーパイプラインを元に戻す
	void End();

	// テクスチャをセットする
	// pTex が nullptr の場合は 1x1 白テクスチャで代替する
	void SetTex(const KdTexture* a_pTex);

	// ディゾルブ用ノイズテクスチャをセットする(PS t1)
	// pTex が nullptr の場合は 1x1 白テクスチャで代替する(dissolveEnabled=falseなら未使用)
	void SetNoiseTex(const KdTexture* a_pTex);

	//----------------------------------------------------------
	// WriteEmitterCb() へ渡す描画の設定。既定値は「特殊な効果のないビルボード」
	//----------------------------------------------------------
	struct EmitterDrawDesc
	{
		Math::Matrix         viewMat;
		int                  aliveCnt = 0;
		const KdTexture*     pTex = nullptr;			// nullptr ならアスペクト比 1.0 とする

		// Billboard / Stretch の切り替え。stretch* は Stretch 時、
		// isGroundPlaneAligned は Billboard 時(板ポリをXZ平面へ寝かせる)だけ使う
		Particle::RenderMode renderMode = Particle::RenderMode::Billboard;
		float                stretchLenScale = 0.0f;
		bool                 isStretchWorldAligned = false;
		bool                 isGroundPlaneAligned = false;

		// 距離フォグ・高さフォグの受け方。既定はフォグを受けない
		Particle::FogMode    fogMode = Particle::FogMode::None;

		// 光を受ける割合(0〜1)。既定は光を受けない
		float                lightResponse = 0.0f;

		// ソフトパーティクルと近距離フェード。0は無効。
		// isSceneDepthValid は、3Dシーンの深度のコピーを TEX_SLOT_SCENE_DEPTH へ結合した時だけtrue
		float                softDist = 0.0f;
		float                nearFadeDist = 0.0f;
		bool                 isSceneDepthValid = false;

		Particle::DissolveParams dissolve;

		// 寿命に対するカーブ。nullptr ならLUTを使わず2点補間になる
		const std::array<float, Particle::CURVE_LUT_SAMPLES>* pSizeCurveLut = nullptr;
		const std::array<float, Particle::CURVE_LUT_SAMPLES>* pAlphaCurveLut = nullptr;
		const std::array<float, Particle::CURVE_LUT_SAMPLES>* pRotCurveLut = nullptr;

		Particle::GlitchParams glitch;
	};

	// エミッターの定数バッファをGPUへ書き込む
	void WriteEmitterCb(const EmitterDrawDesc& a_desc);

	// Draw を発行する
	// count … 描画するパーティクル数
	void DrawParticles(int a_count) const;

	// このフレームで粒子の描画を発行した回数(BeginFrame()で0に戻る)
	int GetFrameDrawCallCnt() const { return m_frameDrawCallCnt; }


private:

	//----------------------------------------------------------
	// StructuredBuffer 作成ヘルパー
	//----------------------------------------------------------
	bool CreateStructuredBuf(UINT a_elementSize, UINT a_elementCnt);

	//----------------------------------------------------------
	// D3D リソース
	//----------------------------------------------------------
	Microsoft::WRL::ComPtr<ID3D11VertexShader> m_pVs;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pPs;

	// StructuredBuffer 本体と SRV
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_pSb;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_pSrv;

	// 確保済み要素数（リサイズ判定用）
	UINT						m_sbCapacity = 0;

	//----------------------------------------------------------
	// リングバッファの状態。
	// 直前のUploadParticles()が書いた開始位置はWriteEmitterCB()が拾い、
	// 頂点シェーダーへ渡す(そこを起点に読ませる)
	//----------------------------------------------------------
	UINT						m_sbWriteOffset = 0;
	UINT						m_lastUploadStartIdx = 0;
	bool						m_isDiscardNeeded = true;

	// このフレームで粒子の描画を発行した回数。フレーム全体の描画数(KdFrameProfiler)には
	// 含まれないため、画面全体の描画呼び出しを数えるときはこちらも足す
	mutable int					m_frameDrawCallCnt = 0;

	// 1フレーム分の粒子をまとめて置ける大きさ。
	// 実測のピークは1フレーム約5600粒子で、余裕を持たせてある
	static constexpr UINT		RING_CAPACITY = 65536;

	// エミッター定数バッファ
	KdConstantBuffer<Particle::cbEmitter> m_cbEmitter;
};

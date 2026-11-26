#pragma once

//=============================================================
// GPUパーティクル用の粒子プール(free list + Emit CS + Update CS + 描画)
// ・固定長の RWStructuredBuffer<ParticleData> をシステム全体で1本共有する
//   (エミッターノード単位ではなくシステム全体でプールする方針)
// ・空きスロット管理は ConsumeStructuredBuffer<uint> による free list
// ・Emit CS が free list から空きスロットを確保し初期化する
// ・Update CS が生存中パーティクルを更新し、死亡したスロットを
//   free list へ返却する
// ・Draw() は既存の ParticleShader を流用し、プールのSRVを直接バインドして描画する
// ・現時点では既存の ParticleEffect/ParticleManager(CPUシミュレーション)とは
//   未接続の独立した基盤。実際のエミッター機能との統合は今後のステップで行う
//=============================================================

#include "Application/Effect/Particle/ParticleConst.h"

namespace Particle
{

	class ParticleGpuPool
	{
	public:

		ParticleGpuPool() = default;

		ParticleGpuPool(const ParticleGpuPool&) = delete;
		ParticleGpuPool& operator=(const ParticleGpuPool&) = delete;

		// capacity … プール全体の最大粒子数
		bool Init(UINT a_capacity);

		void Release();

		// free list から count 個ぶんスロットを確保し、Emit CS で初期化する
		// テスト用の最小初期化のみ行う(free listのセルフテスト専用。ParticleEmit_CS.hlsl)
		void Emit(UINT a_count);

		// EmitterParam に基づいて count 個ぶんスロットを確保し初期化する
		// CPU側 ParticleEffect::Emit() と同じ分布になるよう
		// ParticleEmitReal_CS.hlsl で位置・速度・寿命・色・サイズ・回転を計算する
		// worldPos … エミッターのワールド座標
		void EmitFromParam(UINT a_count, const EmitterParam& a_param, const Math::Vector3& a_worldPos);

		// プール全スロットを走査し、生存中のパーティクルを更新する
		// 寿命が尽きたスロットは自動的に free list へ返却される
		void Update(float a_dt);

		// コリジョン。次回以降のUpdate()呼び出しで参照する平面を設定する
		// (CPU側ParticleEffect::SetColPlanes()と同じ意味論)。planesはCOLLISION_MAX_PLANES件まで
		void SetColPlanes(bool a_isEnabled, float a_bounciness, float a_friction, const std::vector<CollisionPlane>& a_planes);

		// 現在の生存パーティクル数(概算)をCPUへ読み出す
		// CopyStructureCount + STAGING読み出しのため同期待ちが発生する。
		// デバッグ表示等の低頻度用途に限定する
		UINT GetAliveCntApprox();

		// 現在の生存パーティクル数を、GPUを待たずに読む(数フレーム前の値)。
		// 毎フレーム呼んでも同期待ちが起きないため、負荷の計測中や常時表示に使える。
		// 結果が届くまでは前回の値を返す
		UINT GetAliveCntDeferred();

		// 描画の共通設定
		struct DrawDesc
		{
			Math::Matrix     viewMat;
			const KdTexture* pTex = nullptr;			// 描画に使うテクスチャ(nullptrで白1x1)
			BlendMode        blendMode = BlendMode::Add;
			float            time = 0.0f;				// 経過時間(秒)。リボンのUVスクロールやビームのノイズの位相に使う

			// Draw() だけが使う。Billboard/Stretch(VS側の描画方式切り替え)と、Stretch時の伸びの倍率
			RenderMode       renderMode = RenderMode::Billboard;
			float            stretchLenScale = 0.0f;
		};

		// GPUプールをそのまま既存の ParticleShader で描画する
		// (ParticleShader::Begin の pOverrideSrv 経由でプールのSRVを直接バインドする)
		// Ribbon/BeamはDrawRibbon()/DrawBeam()を使う
		void Draw(const DrawDesc& a_desc);

		// リボン(トレイル)のGPU化。毎フレーム、pool->Update()の直後
		// (emitterのUpdate()/Emitより前)に呼ぶこと。パーティクル死亡時に履歴を
		// リセットしてから次の再利用に備える必要があるため、順序が重要
		// (詳細はRibbonTrailUpdate_CS.hlsl参照)
		void UpdateRibbonTrail(float a_dt, const EmitterParam& a_param);

		// リボン(トレイル)のGPU描画
		// UpdateRibbonTrail()で積み上げた履歴をRibbonGpuBuild_CS.hlslで
		// RibbonVertexDataへ変換し、RibbonShaderでそのまま描画する
		void DrawRibbon(const DrawDesc& a_desc, const EmitterParam& a_param);

		// ビーム/レーザーのGPU描画
		// プール全パーティクルからBeamGpuBuild_CS.hlslでRibbonVertexDataを構築し、
		// RibbonShaderでそのまま描画する(CPU版ParticleEffect::DrawBeam()のGPU版)
		// emitterWorldMat … beamDirectionMode==FixedOffsetの回転・
		//                      速度がほぼ0の場合のフォールバック方向に使う
		void DrawBeam(const DrawDesc& a_desc, const EmitterParam& a_param, const Math::Matrix& a_emitterWorldMat);

		UINT GetCapacity() const { return m_capacity; }

		ID3D11Buffer* GetPoolBuf() const { return m_pPoolBuf.Get(); }

	private:

		// 磁力ベクトル場。次回以降のUpdate()呼び出しで参照するソースを設定する
		// (CPU側ParticleEffect::SetMagnetFieldSrcs()と同じ意味論。呼び出し側が
		// 毎フレーム最新の選択状態で呼び直すこと)。srcsはMAGNET_FIELD_MAX_SOURCES件まで
		void SetMagnetField(bool a_isEnabled, float a_strength, float a_twist, const std::vector<MagnetFieldSource>& a_srcs);

		// ビーム頂点バッファを必要なサイズで(再)確保する
		// segmentCntが前回と変わった場合のみ実際に作り直す
		bool EnsureBeamBuf(UINT a_segmentCnt);

		// リボン(トレイル)用のバッファ群を初回のみ確保する
		bool EnsureRibbonBufs();

		Microsoft::WRL::ComPtr<ID3D11Buffer> m_pPoolBuf;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_pPoolUav;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_pPoolSrv;

		Microsoft::WRL::ComPtr<ID3D11Buffer> m_pFreeListBuf;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_pFreeListUav;

		// 生存パーティクル数のカウンタ(Emit CSでIncrement、Update CSでDecrement)
		Microsoft::WRL::ComPtr<ID3D11Buffer> m_pAliveCounterBuf;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_pAliveCounterUav;

		// GetAliveCntApprox() 用の CopyStructureCount 転送先(4バイト)
		Microsoft::WRL::ComPtr<ID3D11Buffer> m_pCntReadbackBuf;

		// GetAliveCntDeferred() 用。生存数の転送先を数フレームぶん持ち、GPUが書き終えたものから読む
		static constexpr int ALIVE_STAGING_SLOT_CNT = 3;
		Microsoft::WRL::ComPtr<ID3D11Buffer> m_pAliveStagingBufs[ALIVE_STAGING_SLOT_CNT];
		bool m_isAliveStagingPending[ALIVE_STAGING_SLOT_CNT] = {};
		int  m_aliveStagingWriteIdx = 0;
		UINT m_aliveCntDeferred = 0;

		Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_pEmitCs;
		Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_pEmitRealCs;
		Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_pUpdateCs;

		// Emit CS 用の定数バッファ(放出数のみ)
		struct EmitCB
		{
			UINT emitCnt = 0;
			UINT pad[3] = {};
		};

		KdConstantBuffer<EmitCB> m_cbEmit;

		// EmitFromParam() / ParticleEmitReal_CS.hlsl 用の定数バッファ
		// HLSL側cbEmitInitと同レイアウト。16バイト境界を守ること
		struct EmitInitCB
		{
			UINT  emitCnt = 0;
			UINT  shape = 0;
			float shapeRadius = 0.0f;
			UINT  randSeed = 0;

			Math::Vector3 shapeBox = { 0.0f, 0.0f, 0.0f };
			float speedMin = 0.0f;

			float speedMax = 0.0f;
			float lifeMin = 0.0f;
			float lifeMax = 0.0f;
			float sizeStart = 0.0f;

			float sizeEnd = 0.0f;
			float rotSpeedMin = 0.0f;
			float rotSpeedMax = 0.0f;
			float emissiveIntensity = 0.0f;

			Math::Vector4 colorStart = { 1.0f, 1.0f, 1.0f, 1.0f };
			Math::Vector4 colorEnd = { 1.0f, 1.0f, 1.0f, 0.0f };

			Math::Vector3 emitterWorldPos = { 0.0f, 0.0f, 0.0f };
			float pad = 0.0f;

			Math::Vector3 forceGravity = { 0.0f, 0.0f, 0.0f };
			float pad2 = 0.0f;
		};

		KdConstantBuffer<EmitInitCB> m_cbEmitInit;

		// EmitFromParam() の呼び出しごとに変える乱数シード
		UINT m_emitSeed = 0;

		// Update CS 用の定数バッファ。ParticleUpdate_CS.hlsl::cbUpdate と同レイアウト
		struct UpdateCB
		{
			float dt = 0.0f;
			UINT  capacity = 0;
			UINT  magnetFieldEnabled = 0;
			UINT  magnetSrcCnt = 0;

			float magnetFieldStrength = 0.0f;
			float magnetFieldTwist = 0.0f;
			float pad[2] = {};

			Math::Vector4 magnetSrcs[MAGNET_FIELD_MAX_SOURCES] = {};	// xyz=位置、w=半径

			UINT  colEnabled = 0;
			UINT  colPlaneCnt = 0;
			float colBounciness = 0.0f;
			float colFriction = 0.0f;

			Math::Vector4 colPlanePositions[COLLISION_MAX_PLANES] = {};	// xyz=平面上の1点
			Math::Vector4 colPlaneNormals[COLLISION_MAX_PLANES] = {};	// xyz=法線
		};

		KdConstantBuffer<UpdateCB> m_cbUpdate;

		UINT m_capacity = 0;

		// SetMagnetField()で設定された値(Update()呼び出し時にcbUpdateへ反映する)
		bool  m_isMagnetFieldEnabled = false;
		float m_magnetFieldStrength = 0.0f;
		float m_magnetFieldTwist = 0.0f;
		std::vector<MagnetFieldSource> m_magnetFieldSrcs;

		// SetColPlanes()で設定された値(Update()呼び出し時にcbUpdateへ反映する)
		bool  m_isColEnabled = false;
		float m_colBounciness = 0.0f;
		float m_colFriction = 0.0f;
		std::vector<CollisionPlane> m_colPlanes;

		//----------------------------------------------------------
		// ビームGPU化
		//----------------------------------------------------------
		Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_pBeamBuildCs;

		Microsoft::WRL::ComPtr<ID3D11Buffer> m_pBeamVertexBuf;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_pBeamVertexUav;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_pBeamVertexSrv;

		// 現在確保済みのbeamSegmentCount(0=未確保)。要求値が変わったら作り直す
		UINT m_beamSegmentCntAllocated = 0;

		// BeamGpuBuild_CS.hlsl::cbBeamBuild と同レイアウト。16バイト境界を守ること
		struct BeamBuildCB
		{
			UINT  capacity = 0;
			UINT  segmentCnt = 0;
			UINT  dirMode = 0;
			float beamLen = 0.0f;

			Math::Vector3 fixedOffsetWorld = { 0.0f, 0.0f, 0.0f };
			float noiseAmplitude = 0.0f;

			Math::Vector3 fallbackDir = { 0.0f, 0.0f, 1.0f };
			float noiseFreq = 0.0f;

			float time = 0.0f;
			float ribbonWidthStart = 0.0f;
			float ribbonWidthEnd = 0.0f;
			float pad = 0.0f;
		};

		KdConstantBuffer<BeamBuildCB> m_cbBeamBuild;

		//----------------------------------------------------------
		// リボン(トレイル)GPU化
		//----------------------------------------------------------
		Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_pRibbonTrailUpdateCs;
		Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_pRibbonBuildCs;

		// 履歴点(capacity * RIBBON_GPU_MAX_POINTS 要素)
		Microsoft::WRL::ComPtr<ID3D11Buffer> m_pRibbonPointsBuf;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_pRibbonPointsUav;

		// パーティクルごとの履歴状態(capacity要素)
		Microsoft::WRL::ComPtr<ID3D11Buffer> m_pRibbonStateBuf;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_pRibbonStateUav;

		// RibbonShaderへ渡す描画用頂点バッファ(capacity * RIBBON_GPU_MAX_POINTS 要素)
		Microsoft::WRL::ComPtr<ID3D11Buffer> m_pRibbonVertexBuf;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_pRibbonVertexUav;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_pRibbonVertexSrv;

		bool m_isRibbonBufsReady = false;

		// RibbonTrailUpdate_CS.hlsl::cbRibbonTrailUpdate と同レイアウト
		struct RibbonTrailUpdateCB
		{
			UINT  capacity = 0;
			float dt = 0.0f;
			float ribbonLifeTime = 0.0f;
			float ribbonMinPointDist = 0.0f;
		};

		KdConstantBuffer<RibbonTrailUpdateCB> m_cbRibbonTrailUpdate;

		// RibbonGpuBuild_CS.hlsl::cbRibbonBuild と同レイアウト
		struct RibbonBuildCB
		{
			UINT  capacity = 0;
			float ribbonLifeTime = 0.0f;
			float ribbonWidthStart = 0.0f;
			float ribbonWidthEnd = 0.0f;
		};

		KdConstantBuffer<RibbonBuildCB> m_cbRibbonBuild;

		// 放出・更新・ビーム/リボン生成のコンピュートシェーダーを作る
		bool CreateComputeShaders();

		// 各パスの定数バッファを作る
		bool CreateConstantBufs();

		// パーティクルのプール本体・生存数カウンタ・free list を作る(失敗したら解放してfalse)
		bool CreatePoolBufs(UINT a_capacity);
		// CreatePoolBufs()の一部。失敗したらfalseを返し、解放は呼び出し側で行う
		bool CreatePoolMainBufs(UINT a_capacity);
		bool CreateAliveCntReadbackBufs();
	};

}	// namespace Particle

#pragma once
#include <array>
#include <cstdint>
#include "Framework/Shader/StandardShader/KdStandardShader.h"

//=============================================================
// 【このクラスが存在する理由】
//   「影 → Lit → デカール → UnLit → Effect → Bright」という描画パスの
//   中身(カリング・描画順ソート・GPUインスタンシング)を1箇所へ集約する。
//   同じ手順がゲームのシーン・MapEditorのシーン描画・MapEditorの
//   プレビュービューポートの3箇所に複製されており、片方だけ改善されて
//   もう片方が取り残される状態になっていたため。
//
// 【使い方】
//   1. Begin() でフレームの判定を初期化する(カメラを渡すと視錐台カリングが有効になる)
//   2. Add() で描画対象を登録する(登録時に可視判定を1度だけ行う)
//   3. 必要なパスを呼ぶ。パスを呼ぶ順序は呼び出し側が決める
//      (ゲームとエディターで順序が異なるため、ここでは固定しない)
//=============================================================

class KdForwardPlus;

class KdDrawPassRenderer
{
public:
	KdDrawPassRenderer();
	~KdDrawPassRenderer();

	//----------------------------------------------------------
	// フレームの開始
	// pCam … 視錐台カリングに使うカメラ。nullptrならシェーダーのカメラ情報を使う
	//----------------------------------------------------------
	void Begin(const KdCamera* a_pCam);

	// 直前に描いたカメラの視錐台をワールド空間で求める(シェーダーのカメラ情報から引く)。
	// ムービー中のカメラも含め、実際に画面へ映っているかを描画以外の処理から判定するために使う。
	// カメラ情報がまだ書き込まれていない場合は false
	static bool TryGetShaderCamFrustum(DirectX::BoundingFrustum& a_outFrustum);

	// 描画対象を登録する(可視判定・描画種別の判定はこの時点で済ませる)
	void Add(KdGameObject* a_pObj);

	// 固定影キャッシュを使うシーンだけ有効にする
	void SetStaticShadowCacheEnabled(bool a_isEnabled);

	// シーン切替時に描画対象への参照とモデル単位のキャッシュを破棄する
	void ResetSceneState();

	bool IsStaticShadowCacheEnabled() const { return m_isStaticShadowCacheEnabled; }
	bool IsStaticShadowCacheBuilt() const { return m_isStaticShadowCacheBuilt; }

	// [幅,高さ,near,far]。固定影マップ生成時のライト空間範囲。
	const std::array<float, 4>& GetStaticShadowProjRange() const { return m_staticShadowProjRange; }

	//----------------------------------------------------------
	// 各描画パス
	// シェーダーのBegin/Endも含めて呼ぶ
	//----------------------------------------------------------

	// 影を生む オブジェクトを深度マップへ描く
	void DrawStaticShadowPassIfNeeded();
	void DrawShadowPass();
	// 近距離の少数点光源について固定影の初回生成と動的影更新を行う
	void DrawLocalShadowPasses();

	// 陰影のないオブジェクト(背景など)
	void DrawUnLitPass();

	// 深度プリパス。Litパスの直前に呼ぶ。まとめ描きできる不透明物の深度だけを先に書き、
	// Litパスで隠れる画素の照明計算を省く。無効時・資源が無い時は何もしない
	void DrawDepthPrepass();

	// 深度プリパスの使用切替(計測の比較用。保存せず、起動時は有効)
	static void SetDepthPrepassEnabled(bool a_isEnabled) { s_isDepthPrepassEnabled = a_isEnabled; }
	static bool IsDepthPrepassEnabled() { return s_isDepthPrepassEnabled; }

	// 局所光源の影の枠の入れ替えの累計(計測用)。読む側が差分を取って毎秒の回数にする
	struct LocalShadowChurnTotals
	{
		std::uint64_t releasedOutOfViewCnt = 0;	// 灯が視野の外へ出て枠を手放した回数
		std::uint64_t releasedOtherCnt = 0;		// 距離・灯の消滅などで枠を手放した回数
		std::uint64_t staticFaceBuiltCnt = 0;	// 固定影の面を作った数
	};
	static const LocalShadowChurnTotals& GetLocalShadowChurnTotals() { return s_localShadowChurnTotals; }

	// 陰影のあるオブジェクト
	void DrawLitPass();

	// 半透明の遮蔽物(不透明描画の後に遠方から手前へ描く)
	void DrawOccluderPass();

	// デカール(不透明を全て描き終えた後にアルファブレンドで重ねる)
	void DrawDecalPass();

	// エフェクト
	void DrawEffectPass();

	// 自ら光るオブジェクト
	void DrawBrightPass();

	// エディターの視点描画用に、影から自発光までを標準の順で続けて描く
	void DrawAllPassesInOrder();

private:

	//----------------------------------------------------------
	// パスへ入る前に求めた判定結果
	//----------------------------------------------------------
	struct Entry
	{
		KdGameObject*      pObj = nullptr;
		const KdModelData* pModel = nullptr;

		// カリング用の境界球
		DirectX::BoundingSphere worldSphere;
		bool hasWorldSphere = false;

		// 視錐台・距離カリングの結果
		bool isVisibleInFrustum = true;	// 視錐台+見かけの大きさの判定を通過したか
		float distFade = 1.0f;		// 距離・見かけの大きさによる見え方(0=描かない、1=そのまま描く)

		// 影パスへの登録判定
		bool isVisibleInShadow = true;	// 影の生成範囲に入っているか
		bool isStaticShadowCaster = false;	// 固定影キャッシュへ登録する対象か

		// 描画物の分類フラグ
		bool isDecal = false;
		bool isOccluder = false;

		// バッチ描画・遮蔽処理の可否
		bool canBatchLit = false;		// Litパスでまとめ描きしてよいか
		bool canBatchShadow = false;	// 影生成パスでまとめ描きしてよいか
		bool isCamFadeExcluded = false;	// カメラ遮蔽のディザー間引きの対象外か
	};

	// 影の生成パスで、固定影として別に描く物体をどう扱うか
	enum class ShadowCasterFilter
	{
		All,			// すべて
		StaticOnly,		// 固定影の対象だけ
		DynamicOnly,	// 固定影の対象を除く
	};

	struct LocalShadowSlot
	{
		// 影を落とす光源の形状
		Math::Vector3 position = Math::Vector3::Zero;
		float radius = 0.0f;

		// 光源リスト上の対応先
		int pointLightIdx = -1;
		int tubeLightIdx = -1;

		// 指向性光源の照射範囲
		Math::Vector3 direction = Math::Vector3::Zero;
		float length = 0.0f;
		float outerCos = 0.0f;

		// 影の生成進捗とフェード制御
		int builtFaces = 0;
		float fadeSec = 0.0f;
		// 各面の描画・カリング用データ
		Math::Matrix viewProj[KdStandardShader::LOCAL_SHADOW_FACE_COUNT] = {};
		DirectX::BoundingFrustum frusta[KdStandardShader::LOCAL_SHADOW_FACE_COUNT];
	};
	std::array<LocalShadowSlot, KdStandardShader::LOCAL_SHADOW_LIGHT_COUNT> m_localShadowSlots;
	// 光源位置から6方向の透視射影とカリング錐台を作る
	void UpdateLocalShadowViews(LocalShadowSlot& a_slot);
	void UpdateTubeLocalShadowViews(LocalShadowSlot& a_slot);
	// 光源の到達範囲が画面へ掛かるかを、境界の余裕込みで、漏れなく含める側に判定する
	bool IsLocalShadowLightVisible(const Math::Vector3& a_position, float a_influenceRadius) const;
	// 枠を手放した理由(視野の外か、それ以外か)を計測用の累計へ足す
	void CntLocalShadowRelease(const LocalShadowSlot& a_slot) const;
	// 登録候補から点光源4灯とTubeLight2灯を選び、固定影を保持する
	void UpdateLocalShadowSelection();
	void UpdateTubeLocalShadowSelection();
	// 指定面へ影を落とす対象を到達球・透視錐台で絞る
	void BuildLocalShadowList(const LocalShadowSlot& a_slot, int a_face, bool a_isStatic);
	// モデルと材質処理が共通の対象をまとめ描きできる順へ並べる
	void SortModelList();

	// 同一モデルが隣接するようソートした一覧を作る
	// (isDecal/isOccluderの扱いはLitパス専用のため引数で切り替える。filter は影の生成パスだけで使う)
	void BuildSortedList(bool a_isLitPass, ShadowCasterFilter a_filter);

	// ソート済み一覧を走査し、同一モデルの連続区間をまとめて描画する。
	// 戻り値は実際に描いた物体の数(一覧の長さはカリングで落ちるものを含む)
	int DrawSortedList(bool a_isLitPass);

	// インスタンシング対象外の1件を個別に描く。描いたら1を返す
	int DrawSortedEntrySingle(const Entry& a_entry, bool a_isLitPass);

	// a_beginから同じモデルが連続する区間をm_instWorlds等へ集め、区間の終わりの添字を返す
	size_t CollectInstancedRun(size_t a_begin, bool a_isLitPass);

	// 集めた区間をまとめて(1件なら個別に)描き、描いた物体の数を返す
	int DrawInstancedRun(const Entry& a_first, bool a_isLitPass);

	// 個別描画の物体へ距離による見え方(0〜1)を渡してLit描画する
	void DrawLitWithDistFade(KdGameObject& a_obj, float a_distFade);

	// モデルのローカル空間バウンディングスフィアを取得する(モデル単位でキャッシュ)
	const DirectX::BoundingSphere* GetLocalBoundingSphere(const KdModelData* a_pModel);

	// モデルのローカル空間バウンディングボックスを取得する(モデル単位でキャッシュ)。
	// 球では厚みが分からないため、影を落とす価値の判定にはこちらを使う
	const DirectX::BoundingBox* GetLocalBoundingBox(const KdModelData* a_pModel);

	// オブジェクトのワールド空間バウンディングスフィアを求める
	bool TryGetWorldBoundingSphere(const KdGameObject& a_obj, DirectX::BoundingSphere& a_outSphere);

	// 固定影対象全体を覆うライトのビュー射影行列を求める
	bool TryBuildStaticLightViewProj(Math::Matrix& a_outLightViewProj);

	// 固定影対象全体のワールドAABBを求める。対象が1つも無ければfalse
	struct StaticCasterBounds
	{
		Math::Vector3 min;
		Math::Vector3 max;
	};
	[[nodiscard]] std::optional<StaticCasterBounds> TryCalcStaticCasterWorldBounds();

	// 固定影対象の各モデルのローカル境界の角をライト空間へ写し、その範囲を求める
	StaticCasterBounds CalcStaticCasterLightSpaceBounds(const Math::Matrix& a_lightView);

	//----------------------------------------------------------
	// メンバー変数
	//----------------------------------------------------------

	std::vector<Entry> m_entries;

	// 描画順ソート用。m_entriesの添字
	std::vector<size_t> m_sortedList;
	std::vector<size_t> m_occluderList;

	// インスタンシング1回ぶんの作業用
	std::vector<Math::Matrix>  m_instWorlds;
	std::vector<float>         m_instFades;	// m_instWorldsと同じ添字の、距離による見え方
	std::vector<KdGameObject*> m_instObjs;

	// モデルごとのローカル空間バウンディングスフィア。
	// 全メッシュノードの走査を毎フレーム行わずに済ませるためのキャッシュ。
	// シーンをまたいで残らないよう、インスタンスのメンバーとして持つ
	std::unordered_map<const KdModelData*, DirectX::BoundingSphere> m_boundsCache;

	// 同・バウンディングボックス版(影を落とす価値の判定に使う)
	std::unordered_map<const KdModelData*, DirectX::BoundingBox> m_boxCache;

	DirectX::BoundingFrustum m_viewFrustum;

	// 深度シェーダーが使うライトのビュー射影行列。影範囲の判定をこれ1つで行う
	Math::Matrix m_lightViewProj;

	Math::Vector3 m_camPos = Math::Vector3::Zero;

	std::unique_ptr<KdForwardPlus> m_upForwardPlus;

	static inline bool s_isDepthPrepassEnabled = true;

	// 局所光源の影の枠の入れ替えの累計(計測用)
	static inline LocalShadowChurnTotals s_localShadowChurnTotals;

	// プリパスが作ったLit用の並び順を、続くLitパスで作り直さず使うか
	bool m_isLitListPrepared = false;
	bool m_hasViewFrustum = false;
	bool m_hasLightViewProj = false;

	bool m_isStaticShadowCacheEnabled = false;
	bool m_isStaticShadowCacheBuilt = false;
	Math::Matrix m_staticShadowViewProj = Math::Matrix::Identity;
	std::array<float, 4> m_staticShadowProjRange = {};
};

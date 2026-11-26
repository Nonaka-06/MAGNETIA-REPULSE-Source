#include "Application/Render/KdDrawPassRenderer.h"
#include "Application/Render/KdForwardPlus.h"
#include "Application/main.h"
#include "Application/System/Utility/KdModelBoundsUtil.h"


namespace
{
	constexpr float LOCAL_SHADOW_SELECT_DISTANCE = 32.0f;
	constexpr float LOCAL_SHADOW_FRUSTUM_MARGIN = 2.0f;
	constexpr float LOCAL_SHADOW_RELEASE_DISTANCE = 40.0f;
	constexpr float LOCAL_SHADOW_FADE_BEGIN_DISTANCE = 25.0f;
	constexpr float LOCAL_SHADOW_FADE_SECONDS = 0.5f;

	//----------------------------------------------------------
	// 遠くの小さいオブジェクトを描画から省く(スモールオブジェクトカリング)。
	// 距離だけで省くと遠くても大きく映る建物まで消えるため、画面上の見かけの大きさで判定する。
	// 「バウンディング球の半径 ÷ 距離」は画面上の大きさに比例し、
	// 1080p・垂直画角60度ではおおよそ ×1871 ピクセルになる。
	// 街路の小物を高密度に置いているため、約19ピクセル以下になるものは描かない。
	// 境界で急に消えないよう、結果は0(描かない)〜1(そのまま描く)の見え方で返し、
	// 描画側がディザーで間引く。見え方が0になる位置が、描く/描かないの境界になる
	//----------------------------------------------------------
	float CalcDistVisibility(const DirectX::BoundingSphere& a_worldSphere, const Math::Vector3& a_camPos)
	{
		constexpr float MIN_SCREEN_SIZE_RATIO = 0.0100f;

		// 見かけの大きさがこの倍率に達すると完全に見える(閾値で0、この倍率で1)
		constexpr float SIZE_FADE_FULL_RATIO = 1.5f;

		// この距離までは大きさに関わらず必ず描く。
		// 足元やすぐ近くの小物が消えると気づかれやすいため。
		//
		// 【負荷に最も効く値】この距離の中にあるオブジェクトは、画面上で何ピクセルだろうと
		// 必ず描かれる。導線上では40mの中に190〜400件が入っており、
		// 描画対象の大半をこれが占めていた。22mでおよそ半分以下になる。
		// プレイヤーの周囲22mは十分な余裕があり、小物が消える様子は見えにくい
		constexpr float ALWAYS_DRAW_DISTANCE = 22.0f;

		// 必ず描く距離の手前から薄くし始める幅。描く範囲の内側に取り、描く件数は増やさない
		constexpr float ALWAYS_DRAW_FADE_WIDTH = 3.0f;

		const Math::Vector3 center(a_worldSphere.Center.x, a_worldSphere.Center.y, a_worldSphere.Center.z);
		const float distance = (center - a_camPos).Length();

		const float nearVisibility = std::clamp(
			(ALWAYS_DRAW_DISTANCE - distance) / ALWAYS_DRAW_FADE_WIDTH, 0.0f, 1.0f);

		const float sizeRatio = a_worldSphere.Radius / std::max(distance, 0.0001f);
		const float sizeVisibility = std::clamp(
			(sizeRatio - MIN_SCREEN_SIZE_RATIO) / (MIN_SCREEN_SIZE_RATIO * (SIZE_FADE_FULL_RATIO - 1.0f)),
			0.0f, 1.0f);

		return std::max(nearVisibility, sizeVisibility);
	}

	// ワールドのY方向にどれだけの高さを持つか。
	// 回転していても正しく評価できるよう、各軸の高さ成分を合成する
	float CalcWorldHeight(const DirectX::BoundingBox& a_localBox, const Math::Matrix& a_world)
	{
		return (std::abs(a_localBox.Extents.x * a_world._12) +
			std::abs(a_localBox.Extents.y * a_world._22) +
			std::abs(a_localBox.Extents.z * a_world._32)) * 2.0f;
	}

	// 地面に平たく置いた小物(布・紙・ゴミ)を、距離で早めに薄くして消す。
	// 件数が多く三角形は少ないため、描画準備とドローの負荷だけが大きい。
	// 道路や歩道のような平たくても大きい物は、半径の上限で対象から外す
	float CalcFlatPropVisibility(const DirectX::BoundingSphere& a_worldSphere, float a_worldHeight,
		const Math::Vector3& a_camPos)
	{
		constexpr float FLAT_MAX_HEIGHT = 0.35f;
		constexpr float FLAT_MAX_RADIUS = 3.0f;
		constexpr float FLAT_FADE_BEGIN_DISTANCE = 30.0f;
		constexpr float FLAT_FADE_END_DISTANCE = 40.0f;

		if (a_worldHeight >= FLAT_MAX_HEIGHT || a_worldSphere.Radius >= FLAT_MAX_RADIUS)
		{
			return 1.0f;
		}

		const Math::Vector3 center(a_worldSphere.Center.x, a_worldSphere.Center.y, a_worldSphere.Center.z);
		const float distance = (center - a_camPos).Length();

		return std::clamp(
			(FLAT_FADE_END_DISTANCE - distance) / (FLAT_FADE_END_DISTANCE - FLAT_FADE_BEGIN_DISTANCE),
			0.0f, 1.0f);
	}

	// 影を落とす価値がある形状か。
	// 地面に落ちた紙や布のような平たい小物まで深度マップへ描くと、影パスのドローコールが
	// 通常描画とほぼ同数まで膨らむ。平たい物は影を落としても地面と重なって見えない。
	// 【重要】差し渡しの大きさで判定しないこと(地面の布は2.4mと大きく、ドラム缶は0.8mと小さい)。
	// 判定は高さ(厚み)で行う
	bool IsWorthCastingShadow(const DirectX::BoundingBox& a_localBox, const Math::Matrix& a_world)
	{
		constexpr float MIN_SHADOW_CASTER_HEIGHT = 0.35f;

		return CalcWorldHeight(a_localBox, a_world) >= MIN_SHADOW_CASTER_HEIGHT;
	}

	// 平行光の影生成範囲を、ライトのビュー射影行列そのもので判定する。
	// 影はカメラ周辺の限られた範囲にしか作られないため、範囲外の物は深度マップへ描かない。
	// 行列は深度シェーダーが実際に使っている cbLight::DirLight_mVP を読むので、
	// 影の設定を変えても判定が自動で追従する。
	// 範囲はライト方向に寝かせた細長い箱で、ワールドAABBに直すと各辺が200m超まで膨らみ
	// ほとんど除外できない。そのため判定対象の球をライトのクリップ空間へ移して
	// [-1,1]x[-1,1]x[0,1] と比べる。この箱の外はGPUがクリップするので、
	// ここで落とした物は元々深度マップへ1画素も書かない(取りこぼしが起きない)。
	bool TryGetLightViewProj(Math::Matrix& a_outLightViewProj)
	{
		const Math::Matrix& lightVp = KdShaderManager::Instance().GetLightCB().DirLight_mVP;

		// 行列が退化している(まだ影の設定が書き込まれていない等)場合はカリングしない。
		// 行列式は影の範囲が広いほど正当に小さくなるため(ゲーム中の範囲75m・奥行192mでも
		// ごく小さい値になる)、大きさでのしきい値は設けず0と非有限値だけを弾く。
		const float determinant = lightVp.Determinant();
		if (!std::isfinite(determinant) || determinant == 0.0f) { return false; }

		a_outLightViewProj = lightVp;

		return true;
	}

	//---------------------------------------------------------
	// TryGetViewFrustum
	// 実際に描いているカメラの視錐台をワールド空間で求める。
	//
	// 【なぜ定数バッファから引くか】
	//   ゲーム側のシーンは KdCamera を Begin() へ渡しておらず、
	//   カメラの行列はシェーダー用の定数バッファへ直接書かれている。
	//   ライト行列を同じ場所から引いているのと揃える
	//---------------------------------------------------------
	struct ViewFrustum
	{
		DirectX::BoundingFrustum frustum;
		Math::Vector3 camPos;
	};

	std::optional<ViewFrustum> TryGetViewFrustum()
	{
		const KdShaderManager::cbCamera& camCb = KdShaderManager::Instance().GetCameraCB();

		// まだ書き込まれていない場合はカリングしない(誤って消さない側へ倒す)
		const float projDeterminant = camCb.mProj.Determinant();
		if (!std::isfinite(projDeterminant) || projDeterminant == 0.0f) { return std::nullopt; }

		const float viewDeterminant = camCb.mView.Determinant();
		if (!std::isfinite(viewDeterminant) || viewDeterminant == 0.0f) { return std::nullopt; }

		// CreateFromMatrix()はビュー空間の錐台を作るため、
		// ビュー行列の逆行列(カメラのワールド行列)でワールド空間へ移す
		const Math::Matrix camWorld = camCb.mView.Invert();

		ViewFrustum view;
		DirectX::BoundingFrustum::CreateFromMatrix(view.frustum, camCb.mProj);
		view.frustum.Transform(view.frustum, camWorld);

		view.camPos = camWorld.Translation();

		return view;
	}

	// 球がライトのクリップ空間の箱に少しでも掛かるか。
	//
	// Vector3::Transform(v, M) は行ベクトル v に M を掛けるため、
	// クリップ座標の各成分の勾配は M の「列」になる。半径をその長さで
	// 伸ばせば、行列の行と列の並びを取り違えずに球の広がりを扱える
	// (並びの取り違えで影が消える事故を過去に起こしているため、
	//  軸やクォータニオンを直接読まずに済む形にしている)。
	bool IsSphereInLightClip(const Math::Matrix& a_lightVp, const DirectX::BoundingSphere& a_worldSphere)
	{
		const Math::Vector3 center(
			a_worldSphere.Center.x, a_worldSphere.Center.y, a_worldSphere.Center.z);

		const Math::Vector3 clipCenter = Math::Vector3::Transform(center, a_lightVp);

		const float radius = a_worldSphere.Radius;
		const float radiusX = radius * Math::Vector3(a_lightVp._11, a_lightVp._21, a_lightVp._31).Length();
		const float radiusY = radius * Math::Vector3(a_lightVp._12, a_lightVp._22, a_lightVp._32).Length();
		const float radiusZ = radius * Math::Vector3(a_lightVp._13, a_lightVp._23, a_lightVp._33).Length();

		if (std::abs(clipCenter.x) > 1.0f + radiusX) { return false; }
		if (std::abs(clipCenter.y) > 1.0f + radiusY) { return false; }
		if (clipCenter.z + radiusZ < 0.0f) { return false; }
		if (clipCenter.z - radiusZ > 1.0f) { return false; }

		return true;
	}
}

bool KdDrawPassRenderer::TryGetShaderCamFrustum(DirectX::BoundingFrustum& a_outFrustum)
{
	const auto view = TryGetViewFrustum();
	if (!view) { return false; }

	a_outFrustum = view->frustum;
	return true;
}

KdDrawPassRenderer::KdDrawPassRenderer() : m_upForwardPlus(std::make_unique<KdForwardPlus>()) {}
KdDrawPassRenderer::~KdDrawPassRenderer() = default;

void KdDrawPassRenderer::Begin(const KdCamera* a_pCam)
{
	m_upForwardPlus->BeginFrame();
	m_entries.clear();

	// カメラの視錐台をワールド空間で求める。
	// CreateFromMatrix()は射影行列からビュー空間の錐台を作るため、
	// カメラのワールド行列(ビュー行列の逆行列)で変換してワールド空間へ持っていく
	m_hasViewFrustum = false;
	m_camPos = Math::Vector3::Zero;

	if (a_pCam)
	{
		DirectX::BoundingFrustum::CreateFromMatrix(m_viewFrustum, a_pCam->GetProjMatrix());
		m_viewFrustum.Transform(m_viewFrustum, a_pCam->GetCameraMatrix());
		m_hasViewFrustum = true;

		// スモールオブジェクトカリングの距離計算に使う
		m_camPos = a_pCam->GetCameraMatrix().Translation();
	}
	else
	{
		// カメラを渡さない呼び出し側(ゲーム側のシーン)でも効くようにする。
		// 渡っていない間はカリングが一切働かず、画面外の物まで毎フレーム
		// 描いていた
		if (const auto view = TryGetViewFrustum())
		{
			m_viewFrustum = view->frustum;
			m_camPos = view->camPos;
			m_hasViewFrustum = true;
		}
	}

	m_hasLightViewProj = TryGetLightViewProj(m_lightViewProj);
	KdShaderManager::Instance().m_StandardShader.WriteCBLocalShadow({});

	// 前フレームで深度プリパスだけが呼ばれた場合に、古い並びを使わせない
	m_isLitListPrepared = false;
}

void KdDrawPassRenderer::SetStaticShadowCacheEnabled(bool a_isEnabled)
{
	if (m_isStaticShadowCacheEnabled == a_isEnabled)
	{
		return;
	}

	m_isStaticShadowCacheEnabled = a_isEnabled;
	m_staticShadowProjRange = {};
	m_localShadowSlots = {};
	KdShaderManager::Instance().m_StandardShader.ResetLocalShadowCache();
	m_isStaticShadowCacheBuilt = false;
	KdShaderManager::Instance().WriteCBStaticShadowViewProj(
		Math::Matrix::Identity,
		false);
}

void KdDrawPassRenderer::ResetSceneState()
{
	m_upForwardPlus->Release();
	// モデルはシーン終了後に解放されるため、生ポインタをキーにした境界情報を
	// 次回入場へ持ち越すと、同じアドレスへ再配置された別モデルに誤適用される。
	m_entries.clear();
	m_sortedList.clear();
	m_occluderList.clear();
	m_instWorlds.clear();
	m_instFades.clear();
	m_instObjs.clear();
	m_boundsCache.clear();
	m_boxCache.clear();
	m_staticShadowProjRange = {};
	m_localShadowSlots = {};
	KdShaderManager::Instance().m_StandardShader.ResetLocalShadowCache();
	m_hasViewFrustum = false;
	m_hasLightViewProj = false;
	m_isLitListPrepared = false;

	m_isStaticShadowCacheBuilt = false;
	m_staticShadowViewProj = Math::Matrix::Identity;
	KdShaderManager::Instance().m_StandardShader.ClearStaticDepthMap();
	KdShaderManager::Instance().WriteCBStaticShadowViewProj(
		Math::Matrix::Identity,
		false);
}

//=============================================================
// Add
// 各パスで同じ判定を繰り返さないよう、登録時に1度だけ判定する。
// バウンディング球の算出は影の範囲判定と視錐台判定の双方で使うため共有する。
// 判定できないオブジェクト(モデルを持たない等)は誤って消さないよう
// 「常に見える」扱いにする
//=============================================================
void KdDrawPassRenderer::Add(KdGameObject* a_pObj)
{
	if (!a_pObj) { return; }

	Entry entry;
	entry.pObj = a_pObj;
	entry.pModel = a_pObj->GetRenderModelData();
	entry.isStaticShadowCaster = a_pObj->IsStaticShadowCaster();
	entry.isDecal = a_pObj->IsDecalDraw();
	entry.isOccluder = a_pObj->IsOccluderDraw();
	entry.canBatchLit = a_pObj->CanBatchInstancedLit();
	entry.canBatchShadow = a_pObj->CanBatchInstancedShadow();
	entry.isCamFadeExcluded = a_pObj->IsCameraFadeExcluded();

	DirectX::BoundingSphere worldSphere;

	if (TryGetWorldBoundingSphere(*a_pObj, worldSphere))
	{
		entry.worldSphere = worldSphere;
		entry.hasWorldSphere = true;
		if (m_hasLightViewProj)
		{
			entry.isVisibleInShadow = IsSphereInLightClip(m_lightViewProj, worldSphere);
		}

		if (m_hasViewFrustum)
		{
			entry.distFade = CalcDistVisibility(worldSphere, m_camPos);

			if (const DirectX::BoundingBox* pFlatBox = GetLocalBoundingBox(entry.pModel))
			{
				const float flatHeight = CalcWorldHeight(*pFlatBox, a_pObj->GetMatrix());
				entry.distFade = std::min(entry.distFade, CalcFlatPropVisibility(worldSphere, flatHeight, m_camPos));
			}

			entry.isVisibleInFrustum =
				m_viewFrustum.Intersects(worldSphere) && entry.distFade > 0.0f;
		}

		// 影の範囲に入っていても、平たすぎて影が見えないオブジェクトは深度マップへ描かない
		if (entry.isVisibleInShadow)
		{
			if (const DirectX::BoundingBox* pLocalBox = GetLocalBoundingBox(entry.pModel))
			{
				entry.isVisibleInShadow = IsWorthCastingShadow(*pLocalBox, a_pObj->GetMatrix());
			}
		}
	}

	m_entries.push_back(entry);
}

//=============================================================
// DrawStaticShadowPassIfNeeded
// 固定背景の深度だけを初回に生成し、以後は通常影パスを動的対象へ絞る
//=============================================================
void KdDrawPassRenderer::DrawStaticShadowPassIfNeeded()
{
	if (!m_isStaticShadowCacheEnabled || m_isStaticShadowCacheBuilt)
	{
		return;
	}

	Math::Matrix staticViewProj;
	if (!TryBuildStaticLightViewProj(staticViewProj))
	{
		// 固定対象が無い場合はキャッシュを有効にせず、通常の影パスを維持する。
		m_isStaticShadowCacheBuilt = false;
		KdShaderManager::Instance().WriteCBStaticShadowViewProj(
			Math::Matrix::Identity,
			false);
		return;
	}

	KdShaderManager& shaderManager = KdShaderManager::Instance();
	const Math::Matrix dynamicViewProj = shaderManager.GetLightCB().DirLight_mVP;

	// 深度シェーダーは共通のライト行列を読むため、固定影の生成中だけ差し替える。
	shaderManager.WriteCBShadowViewProj(staticViewProj);

	KdStandardShader& standardShader = shaderManager.m_StandardShader;
	standardShader.BeginGenerateStaticDepthMapFromLight();
	{
		BuildSortedList(false, ShadowCasterFilter::StaticOnly);
		DrawSortedList(false);
	}
	standardShader.EndGenerateStaticDepthMapFromLight();

	// 次の通常影パスへ、カメラ追従側の行列を戻す。
	shaderManager.WriteCBShadowViewProj(dynamicViewProj);
	shaderManager.WriteCBStaticShadowViewProj(staticViewProj, true);

	m_staticShadowViewProj = staticViewProj;
	m_isStaticShadowCacheBuilt = true;
}

//=============================================================
// DrawShadowPass
// 街規模のマップでは大半のオブジェクトが影の生成範囲の外にあり、
// 深度マップへ描いても影に一切寄与しないため、範囲外のものは描画を省く
//=============================================================
void KdDrawPassRenderer::DrawShadowPass()
{
	KdShaderManager::Instance().m_StandardShader.BeginGenerateDepthMapFromLight();
	{
		// 固定影キャッシュが完成した後は、通常の影マップへ動的対象だけを描く。
		// キャッシュ無効時や初回生成に失敗した場合は全対象を使う。
		BuildSortedList(false,
			(m_isStaticShadowCacheEnabled && m_isStaticShadowCacheBuilt)
				? ShadowCasterFilter::DynamicOnly : ShadowCasterFilter::All);

		// 深度マップへ実際に描いた物体の数。時間だけでは「1件が重い」のか
		// 「件数が多い」のかを切り分けられないため、件数も残す。
		// リストの長さではなく描いた数を渡す(長さにはカリングで
		// 落ちるものが混ざり、作業量を表さないため)
		KdFrameProfiler::Instance().AddSectionItemCount(
			KdFrameProfiler::Section::ShadowPass, DrawSortedList(false));
	}
	KdShaderManager::Instance().m_StandardShader.EndGenerateDepthMapFromLight();
}

//=============================================================
// 点光源の影生成
//=============================================================
void KdDrawPassRenderer::BuildLocalShadowList(const LocalShadowSlot& a_slot, int a_face, bool a_isStatic)
{
	m_sortedList.clear();
	const DirectX::BoundingSphere influence(a_slot.position, a_slot.radius + std::abs(a_slot.length) * 0.5f);
	for (size_t entryIdx = 0; entryIdx < m_entries.size(); ++entryIdx)
	{
		const Entry& entry = m_entries[entryIdx];
		if (entry.isStaticShadowCaster != a_isStatic || entry.isDecal) { continue; }
		if (entry.hasWorldSphere
			&& (!influence.Intersects(entry.worldSphere) || !a_slot.frusta[a_face].Intersects(entry.worldSphere))) { continue; }
		// 点光源は薄い庇や壁も遮光する。影の有無は各物体のDrawShadow側へ委ねる。
		m_sortedList.push_back(entryIdx);
	}
	SortModelList();
}

void KdDrawPassRenderer::UpdateLocalShadowViews(LocalShadowSlot& a_slot)
{
	const Math::Vector3 directions[] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
	const Math::Vector3 up[] = { {0,1,0}, {0,1,0}, {0,0,-1}, {0,0,1}, {0,1,0}, {0,1,0} };
	const Math::Matrix projection = DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV2, 1.0f, KdStandardShader::LOCAL_SHADOW_NEAR_PLANE, a_slot.radius);
	for (int face = 0; face < KdStandardShader::LOCAL_SHADOW_FACE_COUNT; ++face)
	{
		const Math::Matrix view = DirectX::XMMatrixLookAtLH(a_slot.position, a_slot.position + directions[face], up[face]);
		a_slot.viewProj[face] = view * projection;
		DirectX::BoundingFrustum::CreateFromMatrix(a_slot.frusta[face], projection);
		a_slot.frusta[face].Transform(a_slot.frusta[face], view.Invert());
	}
}

void KdDrawPassRenderer::UpdateTubeLocalShadowViews(LocalShadowSlot& a_slot)
{
	Math::Vector3 direction = a_slot.direction;
	if (direction.LengthSquared() <= 0.0001f) { direction = -Math::Vector3::UnitY; }
	direction.Normalize();
	const Math::Vector3 up = std::abs(direction.y) > 0.95f ? Math::Vector3::UnitZ : Math::Vector3::UnitY;
	const float outerCos = std::clamp(a_slot.outerCos, -0.9961947f, 0.9961947f);
	const float fov = std::clamp(2.0f * std::acosf(outerCos), DirectX::XMConvertToRadians(10.0f), DirectX::XMConvertToRadians(170.0f));
	const Math::Matrix view = DirectX::XMMatrixLookAtLH(a_slot.position, a_slot.position + direction, up);
	const Math::Matrix projection = DirectX::XMMatrixPerspectiveFovLH(fov, 1.0f,
		KdStandardShader::LOCAL_SHADOW_NEAR_PLANE, a_slot.radius);
	a_slot.viewProj[0] = view * projection;
	DirectX::BoundingFrustum::CreateFromMatrix(a_slot.frusta[0], projection);
	a_slot.frusta[0].Transform(a_slot.frusta[0], view.Invert());
}

bool KdDrawPassRenderer::IsLocalShadowLightVisible(const Math::Vector3& a_position, float a_influenceRadius) const
{
	// 光源自体が画面外でも、画面内の受光面へ届く光は影の候補に残す。
	return !m_hasViewFrustum || m_viewFrustum.Intersects(
		DirectX::BoundingSphere(a_position, a_influenceRadius + LOCAL_SHADOW_FRUSTUM_MARGIN));
}

void KdDrawPassRenderer::CntLocalShadowRelease(const LocalShadowSlot& a_slot) const
{
	// 距離で外れたものを先に除き、残りを視野の出入りとして数える
	const bool isWithinRelease = (a_slot.position - m_camPos).LengthSquared()
		<= LOCAL_SHADOW_RELEASE_DISTANCE * LOCAL_SHADOW_RELEASE_DISTANCE;
	if (isWithinRelease && !IsLocalShadowLightVisible(a_slot.position, a_slot.radius + std::abs(a_slot.length) * 0.5f))
	{
		++s_localShadowChurnTotals.releasedOutOfViewCnt;
		return;
	}
	++s_localShadowChurnTotals.releasedOtherCnt;
}

void KdDrawPassRenderer::UpdateLocalShadowSelection()
{
	const auto& manager = KdShaderManager::Instance();
	const auto& ambient = manager.GetAmbientController();
	const auto& lights = manager.GetLightCB();
	const auto& required = ambient.GetRequiredPointShadowCandidates();
	const auto isRequired = [&](int a_index)
	{
		return std::find(required.begin(), required.end(), a_index) != required.end();
	};
	std::vector<int> candidates;
	for (int index : ambient.GetPointShadowCandidates())
	{
		if (index < 0 || index >= lights.PointLight_Num) { continue; }
		const auto& light = lights.PointLights[index];
		const bool isViewAccepted = IsLocalShadowLightVisible(light.Pos, light.Radius);
		if (light.Radius <= KdStandardShader::LOCAL_SHADOW_NEAR_PLANE || light.Color.LengthSquared() <= 0.0f
			|| (!isRequired(index) && (!ambient.IsPointShadowsEnabled() || !m_isStaticShadowCacheEnabled || !isViewAccepted
			|| (light.Pos - m_camPos).LengthSquared() > LOCAL_SHADOW_RELEASE_DISTANCE * LOCAL_SHADOW_RELEASE_DISTANCE))) { continue; }
		candidates.push_back(index);
	}
	std::stable_sort(candidates.begin(), candidates.end(), [&](int a_a, int a_b)
	{
		if (isRequired(a_a) != isRequired(a_b)) { return isRequired(a_a); }
		return (lights.PointLights[a_a].Pos - m_camPos).LengthSquared()
			< (lights.PointLights[a_b].Pos - m_camPos).LengthSquared();
	});

	// 必須灯を先頭の枠へ入れ、残りの枠で環境光のキャッシュを維持する。
	const int requiredCnt = static_cast<int>(std::count_if(candidates.begin(), candidates.end(), isRequired));
	std::vector<int> retained;
	for (int i = 0; i < KdStandardShader::LOCAL_SHADOW_POINT_LIGHT_COUNT; ++i)
	{
		auto& slot = m_localShadowSlots[i];
		int match = -1;
		if (slot.pointLightIdx >= 0)
		{
			for (int index : candidates)
			{
				if ((i < requiredCnt && index != candidates[i]) || (i >= requiredCnt && isRequired(index))) { continue; }
				const auto& light = lights.PointLights[index];
				if (light.Pos == slot.position && light.Radius == slot.radius
					&& std::find(retained.begin(), retained.end(), index) == retained.end()) { match = index; break; }
			}
		}
		if (match < 0)
		{
			if (slot.pointLightIdx >= 0) { CntLocalShadowRelease(slot); }
			slot = {};
		}
		else { slot.pointLightIdx = match; retained.push_back(match); }
	}
	for (int i = 0; i < KdStandardShader::LOCAL_SHADOW_POINT_LIGHT_COUNT; ++i)
	{
		auto& slot = m_localShadowSlots[i];
		if (slot.pointLightIdx >= 0) { continue; }
		for (int index : candidates)
		{
			const auto& light = lights.PointLights[index];
			if (!isRequired(index) && (light.Pos - m_camPos).LengthSquared() > LOCAL_SHADOW_SELECT_DISTANCE * LOCAL_SHADOW_SELECT_DISTANCE) { continue; }
			// 視野外でも保つ設定のときも、新しく選ぶのは見えている灯だけにする
			if (!isRequired(index) && !IsLocalShadowLightVisible(light.Pos, light.Radius)) { continue; }
			const bool isUsed = std::any_of(m_localShadowSlots.begin(), m_localShadowSlots.begin() + KdStandardShader::LOCAL_SHADOW_POINT_LIGHT_COUNT,
				[&](const LocalShadowSlot& a_other) { return a_other.pointLightIdx == index; });
			if (isUsed) { continue; }
			slot.position = light.Pos;
			slot.radius = light.Radius;
			slot.pointLightIdx = index;
			UpdateLocalShadowViews(slot);
			break;
		}
	}
}

void KdDrawPassRenderer::UpdateTubeLocalShadowSelection()
{
	const auto& manager = KdShaderManager::Instance();
	const auto& ambient = manager.GetAmbientController();
	const auto& lights = manager.GetLightCB();
	std::vector<int> candidates;
	for (int index : ambient.GetTubeShadowCandidates())
	{
		if (index < 0 || index >= lights.TubeLight_Num) { continue; }
		const auto& light = lights.TubeLights[index];
		const bool isViewAccepted = IsLocalShadowLightVisible(light.Pos, light.Radius + std::abs(light.Length) * 0.5f);
		const bool isLightDark = light.Radius <= KdStandardShader::LOCAL_SHADOW_NEAR_PLANE
			|| light.Intensity <= 0.0f || light.Color.LengthSquared() <= 0.0f;
		const bool isTooFar = (light.Pos - m_camPos).LengthSquared()
			> LOCAL_SHADOW_RELEASE_DISTANCE * LOCAL_SHADOW_RELEASE_DISTANCE;
		if (isLightDark || !isViewAccepted || isTooFar) { continue; }
		candidates.push_back(index);
	}
	std::stable_sort(candidates.begin(), candidates.end(), [&](int a_a, int a_b)
	{
		return (lights.TubeLights[a_a].Pos - m_camPos).LengthSquared()
			< (lights.TubeLights[a_b].Pos - m_camPos).LengthSquared();
	});

	const int firstSlot = KdStandardShader::LOCAL_SHADOW_POINT_LIGHT_COUNT;
	const int slotEnd = firstSlot + KdStandardShader::LOCAL_SHADOW_TUBE_LIGHT_COUNT;
	std::vector<int> retained;
	for (int i = firstSlot; i < slotEnd; ++i)
	{
		auto& slot = m_localShadowSlots[i];
		int match = -1;
		if (slot.tubeLightIdx >= 0)
		{
			for (int index : candidates)
			{
				const auto& light = lights.TubeLights[index];
				const bool isSameShape = light.Pos == slot.position && light.Radius == slot.radius
					&& light.Direction == slot.direction && light.Length == slot.length && light.OuterCos == slot.outerCos;
				const bool isAlreadyRetained = std::find(retained.begin(), retained.end(), index) != retained.end();
				if (isSameShape && !isAlreadyRetained) { match = index; break; }
			}
		}
		if (match < 0)
		{
			if (slot.tubeLightIdx >= 0) { CntLocalShadowRelease(slot); }
			slot = {};
		}
		else { slot.tubeLightIdx = match; retained.push_back(match); }
	}
	for (int i = firstSlot; i < slotEnd; ++i)
	{
		auto& slot = m_localShadowSlots[i];
		if (slot.tubeLightIdx >= 0) { continue; }
		for (int index : candidates)
		{
			const auto& light = lights.TubeLights[index];
			if ((light.Pos - m_camPos).LengthSquared() > LOCAL_SHADOW_SELECT_DISTANCE * LOCAL_SHADOW_SELECT_DISTANCE) { continue; }
			if (!IsLocalShadowLightVisible(light.Pos, light.Radius + std::abs(light.Length) * 0.5f)) { continue; }
			const bool isUsed = std::any_of(m_localShadowSlots.begin() + firstSlot, m_localShadowSlots.begin() + slotEnd,
				[&](const LocalShadowSlot& a_other) { return a_other.tubeLightIdx == index; });
			if (isUsed) { continue; }
			slot.position = light.Pos;
			slot.radius = light.Radius;
			slot.direction = light.Direction;
			slot.length = light.Length;
			slot.outerCos = light.OuterCos;
			slot.tubeLightIdx = index;
			UpdateTubeLocalShadowViews(slot);
			break;
		}
	}
}

void KdDrawPassRenderer::DrawLocalShadowPasses()
{
	auto& manager = KdShaderManager::Instance();
	auto& shader = manager.m_StandardShader;
	const auto& ambient = manager.GetAmbientController();
	const auto& required = ambient.GetRequiredPointShadowCandidates();
	const auto isRequired = [&](int a_index)
	{
		return a_index >= 0 && std::find(required.begin(), required.end(), a_index) != required.end();
	};
	if (required.empty() && (!m_isStaticShadowCacheEnabled || !m_hasViewFrustum || !ambient.IsPointShadowsEnabled()
		|| (!ambient.IsPointLightsEnabled() && !ambient.IsTubeLightsEnabled()))) { return; }

	const Math::Matrix directionalVp = manager.GetLightCB().DirLight_mVP;
	KdStandardShader::cbLocalShadow result;
	UpdateLocalShadowSelection();
	if (m_isStaticShadowCacheEnabled && ambient.IsPointShadowsEnabled()) { UpdateTubeLocalShadowSelection(); }
	else
	{
		for (int i = KdStandardShader::LOCAL_SHADOW_POINT_LIGHT_COUNT; i < KdStandardShader::LOCAL_SHADOW_LIGHT_COUNT; ++i) { m_localShadowSlots[i] = {}; }
	}

	{
		KdScopedProfileSection profile(KdFrameProfiler::Section::LocalShadowStatic);
		for (int i = 0; i < KdStandardShader::LOCAL_SHADOW_LIGHT_COUNT; ++i)
		{
			auto& slot = m_localShadowSlots[i];
			const bool isTube = i >= KdStandardShader::LOCAL_SHADOW_POINT_LIGHT_COUNT;
			const bool isSelected = isTube ? slot.tubeLightIdx >= 0 : slot.pointLightIdx >= 0;
			if (!isSelected) { continue; }
			++result.SelectedCount;
			const int faceCnt = isTube ? 1 : KdStandardShader::LOCAL_SHADOW_FACE_COUNT;
			if (slot.builtFaces >= faceCnt) { continue; }
			const int buildEnd = isRequired(slot.pointLightIdx) ? faceCnt : slot.builtFaces + 1;
			while (slot.builtFaces < buildEnd)
			{
				const int face = slot.builtFaces;
				if (!shader.BeginGenerateLocalShadow(i, face, true)) { break; }
				manager.WriteCBShadowViewProj(slot.viewProj[face]);
				BuildLocalShadowList(slot, face, true);
				KdFrameProfiler::Instance().AddSectionItemCount(KdFrameProfiler::Section::LocalShadowStatic, DrawSortedList(false));
				shader.EndGenerateLocalShadow();
				++slot.builtFaces;
				++s_localShadowChurnTotals.staticFaceBuiltCnt;
			}
		}
	}

	{
		KdScopedProfileSection profile(KdFrameProfiler::Section::LocalShadowDynamic);
		for (int i = 0; i < KdStandardShader::LOCAL_SHADOW_LIGHT_COUNT; ++i)
		{
			auto& slot = m_localShadowSlots[i];
			const bool isTube = i >= KdStandardShader::LOCAL_SHADOW_POINT_LIGHT_COUNT;
			const bool isSelected = isTube ? slot.tubeLightIdx >= 0 : slot.pointLightIdx >= 0;
			const int faceCnt = isTube ? 1 : KdStandardShader::LOCAL_SHADOW_FACE_COUNT;
			if (!isSelected || slot.builtFaces < faceCnt) { continue; }
			// 視野外で保っている枠は、見えている画素へ光が届かないため動的影を描き直さない
			if (!isRequired(slot.pointLightIdx)
				&& !IsLocalShadowLightVisible(slot.position, slot.radius + std::abs(slot.length) * 0.5f)) { continue; }
			bool isCompleted = true;
			for (int face = 0; face < faceCnt; ++face)
			{
				BuildLocalShadowList(slot, face, false);
				if (face != 0 && m_sortedList.empty()) { continue; }
				if (!shader.BeginGenerateLocalShadow(i, face, false)) { isCompleted = false; break; }
				manager.WriteCBShadowViewProj(slot.viewProj[face]);
				KdFrameProfiler::Instance().AddSectionItemCount(KdFrameProfiler::Section::LocalShadowDynamic, DrawSortedList(false));
				shader.EndGenerateLocalShadow();
			}
			if (!isCompleted) { continue; }
			slot.fadeSec = std::min(LOCAL_SHADOW_FADE_SECONDS, slot.fadeSec + APP.GetUnscaledDeltaTime());
			const float distance = (slot.position - m_camPos).Length();
			auto& light = result.Lights[i];
			light.PointLightIndex = slot.pointLightIdx;
			light.TubeLightIndex = slot.tubeLightIdx;
			light.Strength = isRequired(slot.pointLightIdx) ? 1.0f : std::clamp((LOCAL_SHADOW_RELEASE_DISTANCE - distance) / (LOCAL_SHADOW_RELEASE_DISTANCE - LOCAL_SHADOW_FADE_BEGIN_DISTANCE), 0.0f, 1.0f)
				* (slot.fadeSec / LOCAL_SHADOW_FADE_SECONDS);
			std::copy(std::begin(slot.viewProj), std::end(slot.viewProj), std::begin(light.ViewProj));
			++result.ReadyCount;
		}
	}
	manager.WriteCBShadowViewProj(directionalVp);
	shader.WriteCBLocalShadow(result);
}

void KdDrawPassRenderer::DrawUnLitPass()
{
	KdForwardPlus::Binding forwardPlus(*m_upForwardPlus);
	KdScopedProfileSection profileSection(KdFrameProfiler::Section::ScenePassUnLit);

	int drawnCnt = 0;

	KdShaderManager::Instance().m_StandardShader.BeginUnLit();
	{
		for (const Entry& entry : m_entries)
		{
			if (!entry.isVisibleInFrustum) { continue; }

			entry.pObj->DrawUnLit();
			++drawnCnt;
		}
	}
	KdShaderManager::Instance().m_StandardShader.EndUnLit();

	KdFrameProfiler::Instance().AddSectionItemCount(
		KdFrameProfiler::Section::ScenePassUnLit, drawnCnt);
}

//=============================================================
// DrawLitPass
// 同じモデルを大量に置いた建物などを1体ずつ描くと、マテリアルの切り替えとドローコールが
// オブジェクトごとに発生して重い。同じモデルのオブジェクトが隣り合うよう並べ替え、
// 連続する区間を1回のインスタンシング描画にまとめる。対象外(アニメーション・スキンメッシュ等)は
// 個別に描く(KdGameObject::CanBatchInstancedLit()参照)
//=============================================================
void KdDrawPassRenderer::DrawLitPass()
{
	KdForwardPlus::Binding forwardPlus(*m_upForwardPlus);
	KdScopedProfileSection profileSection(KdFrameProfiler::Section::ScenePassLit);

	int drawnCnt = 0;

	KdShaderManager::Instance().m_StandardShader.BeginLit();
	{
		// プリパスが同じ条件で作った並び順があれば使う(作り直すと束ね方が変わらないことの保証も要らなくなる)
		if (!m_isLitListPrepared)
		{
			BuildSortedList(true, ShadowCasterFilter::All);
		}
		m_isLitListPrepared = false;

		drawnCnt = DrawSortedList(true);
	}
	KdShaderManager::Instance().m_StandardShader.EndLit();

	// Litパスで実際に描いた物体の数。影パスの件数と並べて見ることで、
	// 影のカリングがどれだけ効いているかが分かる
	KdFrameProfiler::Instance().AddSectionItemCount(
		KdFrameProfiler::Section::ScenePassLit, drawnCnt);
}

//=============================================================
// DrawDepthPrepass
// まとめ描き(インスタンシング)で2件以上束ねる物だけ、深度を先に書く。
//
// 【重要】Litパスと同じ束ね方・同じ描画経路にすること。プリパスとLitパスで頂点シェーダーが
// 違うと、同じ頂点でも浮動小数の丸めで深度が僅かにずれ、「手前か同じなら通す」の判定に
// 落ちて穴になる。そのため、Litパスで個別描画になる物(束が1件)はプリパスにも入れない。
// 入れなかったオブジェクトは、Litパスで通常どおり深度テストと深度書き込みを行う
//=============================================================
void KdDrawPassRenderer::DrawDepthPrepass()
{
	if (!s_isDepthPrepassEnabled) { return; }

	auto& standardShader = KdShaderManager::Instance().m_StandardShader;

	KdScopedProfileSection profileSection(KdFrameProfiler::Section::ScenePassPrepass);

	BuildSortedList(true, ShadowCasterFilter::All);
	m_isLitListPrepared = true;

	if (!standardShader.BeginDepthPrepass()) { return; }

	const size_t listSize = m_sortedList.size();
	size_t i = 0;

	while (i < listSize)
	{
		const Entry& first = m_entries[m_sortedList[i]];

		m_instWorlds.clear();
		m_instFades.clear();
		size_t j = i;

		// 束の区切りはDrawSortedList(true)と同じ条件にする
		while (j < listSize)
		{
			const Entry& entry = m_entries[m_sortedList[j]];

			if (entry.pModel != first.pModel || !entry.canBatchLit
				|| entry.isCamFadeExcluded != first.isCamFadeExcluded)
			{
				break;
			}

			if (entry.isVisibleInFrustum)
			{
				m_instWorlds.push_back(entry.pObj->GetMatrix());
				m_instFades.push_back(entry.distFade);
			}
			++j;
		}

		// 区切りが1件も進まなかった(束ねられないオブジェクト)場合は1件だけ進める
		if (j == i)
		{
			++i;
			continue;
		}

		if (m_instWorlds.size() >= 2)
		{
			if (first.isCamFadeExcluded)
			{
				standardShader.SetCameraFadeExclude(true);
			}

			standardShader.DrawModelInstanced(*first.pModel, m_instWorlds,
				kWhiteColor, Math::Vector3::Zero, &m_instFades);
		}

		i = j;
	}

	standardShader.EndDepthPrepass();
}

//=============================================================
// DrawOccluderPass
// 通常のLit描画が終わった後、半透明遮蔽物をアルファ合成する。
// 不透明パスへ混ぜると深度を書き込んでしまうため、専用の状態で描く。
//=============================================================
void KdDrawPassRenderer::DrawOccluderPass()
{
	KdForwardPlus::Binding forwardPlus(*m_upForwardPlus);
	KdScopedProfileSection profileSection(KdFrameProfiler::Section::ScenePassOccluder);

	if (m_occluderList.empty()) { return; }

	// アルファ合成は奥から手前の順が必要。距離が等しい場合は登録順を保つ。
	std::stable_sort(m_occluderList.begin(), m_occluderList.end(),
		[this](size_t a_a, size_t a_b)
		{
			const float aDistSqr =
				(m_entries[a_a].pObj->GetPos() - m_camPos).LengthSquared();
			const float bDistSqr =
				(m_entries[a_b].pObj->GetPos() - m_camPos).LengthSquared();

			return aDistSqr > bDistSqr;
		});

	auto& standardShader = KdShaderManager::Instance().m_StandardShader;
	int drawnCnt = 0;

	standardShader.BeginTranslucentDraw(false, false);
	{
		for (const size_t entryIdx : m_occluderList)
		{
			const Entry& entry = m_entries[entryIdx];
			if (!entry.isVisibleInFrustum) { continue; }

			entry.pObj->DrawLit();
			++drawnCnt;
		}
	}
	standardShader.EndTranslucentDraw();

	KdFrameProfiler::Instance().AddSectionItemCount(
		KdFrameProfiler::Section::ScenePassOccluder, drawnCnt);
}

//=============================================================
// DrawDecalPass
// デカールは板ポリゴンなので、不透明のまま描くと四角い板が見える。不透明物を全て
// 描いた後にアルファブレンド・深度書き込みなしで重ね、デカール同士が消し合わないようにする。
// 平行光・環境光・影を受けるよう Lit パスで描き、ソフトアルファでぼかしも残す
//=============================================================
void KdDrawPassRenderer::DrawDecalPass()
{
	KdForwardPlus::Binding forwardPlus(*m_upForwardPlus);
	// 重ねて描くものの作法(深度書き込みオフ・加算時の環境反射/フォグオフ)は
	// BeginTranslucentDraw()側にまとめてある。
	// デカールはアルファ合成なので、フォグは効かせたままにする
	auto& standardShader = KdShaderManager::Instance().m_StandardShader;

	KdScopedProfileSection profileSection(KdFrameProfiler::Section::ScenePassDecal);

	int drawnCnt = 0;

	standardShader.BeginTranslucentDraw(false, true);
	{
		for (const Entry& entry : m_entries)
		{
			if (!entry.isDecal || !entry.isVisibleInFrustum) { continue; }

			standardShader.ApplyTranslucentObjectParams();

			entry.pObj->DrawDecal();
			++drawnCnt;
		}
	}
	standardShader.EndTranslucentDraw();

	KdFrameProfiler::Instance().AddSectionItemCount(
		KdFrameProfiler::Section::ScenePassDecal, drawnCnt);
}

void KdDrawPassRenderer::DrawEffectPass()
{
	KdForwardPlus::Binding forwardPlus(*m_upForwardPlus);
	KdScopedProfileSection profileSection(KdFrameProfiler::Section::ScenePassEffect);
	int drawnCnt = 0;

	KdShaderManager::Instance().m_StandardShader.BeginUnLit();
	{
		for (const Entry& entry : m_entries)
		{
			if (!entry.isVisibleInFrustum) { continue; }

			entry.pObj->DrawEffect();
			++drawnCnt;
		}
	}
	KdShaderManager::Instance().m_StandardShader.EndUnLit();

	KdFrameProfiler::Instance().AddSectionItemCount(
		KdFrameProfiler::Section::ScenePassEffect, drawnCnt);
}

void KdDrawPassRenderer::DrawBrightPass()
{
	KdForwardPlus::Binding forwardPlus(*m_upForwardPlus);
	KdScopedProfileSection profileSection(KdFrameProfiler::Section::ScenePassBright);
	int drawnCnt = 0;

	KdShaderManager::Instance().m_postProcessShader.BeginBright();
	{
		for (const Entry& entry : m_entries)
		{
			if (!entry.isVisibleInFrustum) { continue; }

			entry.pObj->DrawBright();
			++drawnCnt;
		}
	}
	KdShaderManager::Instance().m_postProcessShader.EndBright();

	KdFrameProfiler::Instance().AddSectionItemCount(
		KdFrameProfiler::Section::ScenePassBright, drawnCnt);
}

void KdDrawPassRenderer::DrawAllPassesInOrder()
{
	DrawShadowPass();
	DrawLitPass();
	DrawDecalPass();
	DrawUnLitPass();
	DrawOccluderPass();
	DrawEffectPass();
	DrawBrightPass();
}

void KdDrawPassRenderer::BuildSortedList(bool a_isLitPass, ShadowCasterFilter a_filter)
{
	const bool isStaticOnly = (a_filter == ShadowCasterFilter::StaticOnly);
	const bool isDynamicOnly = (a_filter == ShadowCasterFilter::DynamicOnly);

	m_sortedList.clear();
	m_occluderList.clear();
	m_sortedList.reserve(m_entries.size());

	for (size_t entryIdx = 0; entryIdx < m_entries.size(); ++entryIdx)
	{
		const Entry& entry = m_entries[entryIdx];
		if (a_isLitPass)
		{
			// デカールは専用パスで描くためここでは扱わない
			if (entry.isDecal) { continue; }

			if (entry.isOccluder)
			{
				m_occluderList.push_back(entryIdx);
				continue;
			}
		}
		else if (isStaticOnly)
		{
			if (!entry.isStaticShadowCaster) { continue; }
		}
		else
		{
			if (isDynamicOnly && entry.isStaticShadowCaster) { continue; }

			if (!entry.isVisibleInShadow)
			{
				continue;
			}
		}

		m_sortedList.push_back(entryIdx);
	}

	SortModelList();
}

void KdDrawPassRenderer::SortModelList()
{
	// 同一モデルが隣接するよう安定ソート(同キー内の描画順は維持される)。
	// 間引きの対象外かどうかはまとめ描き1回で共通のため、これも同じ区間に揃える
	std::stable_sort(m_sortedList.begin(), m_sortedList.end(),
		[this](size_t a_a, size_t a_b)
		{
			const Entry& entryA = m_entries[a_a];
			const Entry& entryB = m_entries[a_b];
			if (entryA.pModel != entryB.pModel)
			{
				return entryA.pModel < entryB.pModel;
			}
			return entryA.isCamFadeExcluded < entryB.isCamFadeExcluded;
		});
}

int KdDrawPassRenderer::DrawSortedList(bool a_isLitPass)
{
	// 実際に描いた物体の数。リストの長さはカリングで落ちるものを含むため、
	// 作業量の指標にはこちらを使う
	int drawnCnt = 0;

	const size_t listSize = m_sortedList.size();
	size_t i = 0;

	while (i < listSize)
	{
		const Entry& first = m_entries[m_sortedList[i]];
		const bool isFirstCanBatch = a_isLitPass ? first.canBatchLit : first.canBatchShadow;

		// インスタンシング対象外のオブジェクトは1件ずつ個別描画する
		if (!isFirstCanBatch)
		{
			drawnCnt += DrawSortedEntrySingle(first, a_isLitPass);
			++i;
			continue;
		}

		const size_t runEnd = CollectInstancedRun(i, a_isLitPass);
		drawnCnt += DrawInstancedRun(first, a_isLitPass);
		i = runEnd;
	}

	return drawnCnt;
}

//=============================================================
// DrawSortedEntrySingle [private]
// 視錐台外ならフラスタムカリングで描画自体をスキップする。
// 影生成パスは範囲判定をリスト構築の時点で済ませている
//=============================================================
int KdDrawPassRenderer::DrawSortedEntrySingle(const Entry& a_entry, bool a_isLitPass)
{
	if (!a_isLitPass)
	{
		a_entry.pObj->GenerateDepthMapFromLight();
		return 1;
	}

	if (a_entry.isVisibleInFrustum)
	{
		DrawLitWithDistFade(*a_entry.pObj, a_entry.distFade);
		return 1;
	}

	return 0;
}

//=============================================================
// CollectInstancedRun [private]
// a_beginから同じモデルデータが連続する区間を集め、区間の終わり(次の先頭)を返す。
// Litパスはこのうちフラスタムカリングを通過したものだけを対象にする
// (m_instObjsはm_instWorldsと同じ添字で対応させ、1件しかない
//  場合の個別描画フォールバックで正しいオブジェクトを引けるようにする)
//=============================================================
size_t KdDrawPassRenderer::CollectInstancedRun(size_t a_begin, bool a_isLitPass)
{
	const Entry& first = m_entries[m_sortedList[a_begin]];
	const KdModelData* pModelKey = first.pModel;
	const size_t listSize = m_sortedList.size();

	m_instWorlds.clear();
	m_instFades.clear();
	m_instObjs.clear();
	size_t j = a_begin;

	while (j < listSize)
	{
		const Entry& entry = m_entries[m_sortedList[j]];
		const bool canBatch = a_isLitPass ? entry.canBatchLit : entry.canBatchShadow;

		if (entry.pModel != pModelKey || !canBatch
			|| (a_isLitPass && entry.isCamFadeExcluded != first.isCamFadeExcluded))
		{
			break;
		}

		if (!a_isLitPass || entry.isVisibleInFrustum)
		{
			m_instWorlds.push_back(entry.pObj->GetMatrix());
			m_instFades.push_back(entry.distFade);
			m_instObjs.push_back(entry.pObj);
		}
		++j;
	}

	return j;
}

//=============================================================
// DrawInstancedRun [private]
// CollectInstancedRun()で集めた区間を描き、描いた物体の数を返す。
// 2件以上まとまった場合のみインスタンシングの恩恵があるため、
// 1件しかない場合は通常の描画にフォールバックする
// (0件=区間の全オブジェクトが視錐台外だった場合は何もしない)
//=============================================================
int KdDrawPassRenderer::DrawInstancedRun(const Entry& a_first, bool a_isLitPass)
{
	auto& standardShader = KdShaderManager::Instance().m_StandardShader;

	if (m_instWorlds.size() >= 2)
	{
		if (a_isLitPass)
		{
			// DrawModelInstanced()の末尾で既定値へ戻るため、戻す処理は要らない
			if (a_first.isCamFadeExcluded)
			{
				standardShader.SetCameraFadeExclude(true);
			}

			standardShader.DrawModelInstanced(*a_first.pModel, m_instWorlds,
				kWhiteColor, Math::Vector3::Zero, &m_instFades);
		}
		else
		{
			standardShader.DrawModelInstancedGenDepth(*a_first.pModel, m_instWorlds);
		}

		return static_cast<int>(m_instWorlds.size());
	}

	if (m_instWorlds.size() == 1)
	{
		if (a_isLitPass)
		{
			DrawLitWithDistFade(*m_instObjs[0], m_instFades[0]);
		}
		else
		{
			m_instObjs[0]->GenerateDepthMapFromLight();
		}

		return 1;
	}

	return 0;
}

//=============================================================
// DrawLitWithDistFade [private]
// 個別描画の物体へ距離による見え方を渡して描く。
// 描画されなかった場合に次の物体へ持ち越さないよう、薄くした時だけ描画後に1へ戻す
//=============================================================
void KdDrawPassRenderer::DrawLitWithDistFade(KdGameObject& a_obj, float a_distFade)
{
	auto& standardShader = KdShaderManager::Instance().m_StandardShader;

	if (a_distFade >= 1.0f)
	{
		a_obj.DrawLit();
		return;
	}

	standardShader.SetDistanceVisibility(a_distFade);
	a_obj.DrawLit();
	standardShader.SetDistanceVisibility(1.0f);
}

//=============================================================
// GetLocalBoundingSphere [private]
// KdModelBoundsUtil::CalcLocalBoundingSphere()は全メッシュノードを
// 走査するため、同じモデルを使うオブジェクトが多い場面で毎フレーム
// 呼び直すのは無駄が大きい。モデル単位でキャッシュする
//=============================================================
const DirectX::BoundingSphere* KdDrawPassRenderer::GetLocalBoundingSphere(const KdModelData* a_pModel)
{
	if (!a_pModel) { return nullptr; }

	const auto it = m_boundsCache.find(a_pModel);
	if (it != m_boundsCache.end())
	{
		return &it->second;
	}

	DirectX::BoundingSphere localSphere;
	if (!KdModelBoundsUtil::CalcLocalBoundingSphere(a_pModel, localSphere))
	{
		return nullptr;
	}

	return &(m_boundsCache[a_pModel] = localSphere);
}

//=============================================================
// GetLocalBoundingBox [private]
// 影を落とす価値があるか(厚みがあるか)の判定に使う。
// 球と同じくモデル単位でキャッシュする
//=============================================================
const DirectX::BoundingBox* KdDrawPassRenderer::GetLocalBoundingBox(const KdModelData* a_pModel)
{
	if (!a_pModel) { return nullptr; }

	const auto it = m_boxCache.find(a_pModel);
	if (it != m_boxCache.end())
	{
		return &it->second;
	}

	DirectX::BoundingBox localBox;
	if (!KdModelBoundsUtil::CalcLocalBoundingBox(a_pModel, localBox))
	{
		return nullptr;
	}

	return &(m_boxCache[a_pModel] = localBox);
}

//=============================================================
// TryBuildStaticLightViewProj [private]
// 固定影対象全体を覆うライト空間の直交射影を作る
//=============================================================
bool KdDrawPassRenderer::TryBuildStaticLightViewProj(Math::Matrix& a_outLightViewProj)
{
	m_staticShadowProjRange = {};
	const auto worldBounds = TryCalcStaticCasterWorldBounds();
	if (!worldBounds)
	{
		return false;
	}
	const Math::Vector3& boundsMin = worldBounds->min;
	const Math::Vector3& boundsMax = worldBounds->max;

	Math::Vector3 lightDir = KdShaderManager::Instance().GetLightCB().DirLight_Dir;
	if (lightDir.LengthSquared() <= 0.0001f)
	{
		return false;
	}
	lightDir.Normalize();

	const Math::Vector3 boundsCenter = (boundsMin + boundsMax) * 0.5f;
	const Math::Vector3 boundsExtents = (boundsMax - boundsMin) * 0.5f;
	const float boundsRadius = std::max(boundsExtents.Length(), 1.0f);

	constexpr float DEPTH_MARGIN = 64.0f;
	constexpr float XY_MARGIN_RATIO = 0.05f;

	Math::Vector3 upVec = Math::Vector3::Up;
	if (std::abs(lightDir.Dot(upVec)) > 0.95f)
	{
		upVec = Math::Vector3::Right;
	}

	const Math::Vector3 lightPos =
		boundsCenter - lightDir * (boundsRadius * 2.0f + DEPTH_MARGIN);
	const Math::Matrix lightView =
		DirectX::XMMatrixLookAtLH(lightPos, boundsCenter, upVec);

	const StaticCasterBounds lightBounds = CalcStaticCasterLightSpaceBounds(lightView);
	const Math::Vector3& lightMin = lightBounds.min;
	const Math::Vector3& lightMax = lightBounds.max;
	const float minX = lightMin.x;
	const float minY = lightMin.y;
	const float minZ = lightMin.z;
	const float maxX = lightMax.x;
	const float maxY = lightMax.y;
	const float maxZ = lightMax.z;

	const float xyExtent = std::max(maxX - minX, maxY - minY);
	const float lightH = std::sqrtf(
		lightDir.x * lightDir.x + lightDir.z * lightDir.z);
	const float lightV = std::max(std::abs(lightDir.y), 0.15f);
	// 建物の頂部から地面へ伸びる影の移動量も範囲へ含める。
	const float shadowReach =
		(boundsMax.y - boundsMin.y) * lightH / lightV;
	const float xyMargin = std::max(
		std::max(2.0f, xyExtent * XY_MARGIN_RATIO), shadowReach);
	const float nearZ = std::max(0.1f, minZ - DEPTH_MARGIN);
	const float farZ = std::max(nearZ + 1.0f, maxZ + DEPTH_MARGIN);

	const Math::Matrix lightProj = DirectX::XMMatrixOrthographicOffCenterLH(
		minX - xyMargin, maxX + xyMargin,
		minY - xyMargin, maxY + xyMargin,
		nearZ, farZ);

	m_staticShadowProjRange = {
		maxX - minX + xyMargin * 2.0f,
		maxY - minY + xyMargin * 2.0f,
		nearZ,
		farZ };
	a_outLightViewProj = lightView * lightProj;
	return true;
}

std::optional<KdDrawPassRenderer::StaticCasterBounds> KdDrawPassRenderer::TryCalcStaticCasterWorldBounds()
{
	StaticCasterBounds bounds;
	bool hasBounds = false;

	for (const Entry& entry : m_entries)
	{
		if (!entry.isStaticShadowCaster || !entry.pModel)
		{
			continue;
		}

		const DirectX::BoundingBox* pLocalBox = GetLocalBoundingBox(entry.pModel);
		if (!pLocalBox)
		{
			continue;
		}

		DirectX::BoundingBox worldBox;
		pLocalBox->Transform(worldBox, entry.pObj->GetMatrix());

		const Math::Vector3 worldMin(
			worldBox.Center.x - worldBox.Extents.x,
			worldBox.Center.y - worldBox.Extents.y,
			worldBox.Center.z - worldBox.Extents.z);
		const Math::Vector3 worldMax(
			worldBox.Center.x + worldBox.Extents.x,
			worldBox.Center.y + worldBox.Extents.y,
			worldBox.Center.z + worldBox.Extents.z);

		if (!hasBounds)
		{
			bounds.min = worldMin;
			bounds.max = worldMax;
			hasBounds = true;
			continue;
		}

		bounds.min = Math::Vector3::Min(bounds.min, worldMin);
		bounds.max = Math::Vector3::Max(bounds.max, worldMax);
	}

	if (!hasBounds) { return std::nullopt; }
	return bounds;
}

//=============================================================
// CalcStaticCasterLightSpaceBounds [private]
// 全体のワールドAABBを再投影すると、離れた物体の間の空間まで範囲が膨らむ。
// 投影する角は各モデルのローカル境界から直接求める
//=============================================================
KdDrawPassRenderer::StaticCasterBounds KdDrawPassRenderer::CalcStaticCasterLightSpaceBounds(
	const Math::Matrix& a_lightView)
{
	StaticCasterBounds bounds;
	bounds.min = Math::Vector3(std::numeric_limits<float>::max());
	bounds.max = Math::Vector3(-std::numeric_limits<float>::max());

	for (const Entry& entry : m_entries)
	{
		if (!entry.isStaticShadowCaster || !entry.pModel) { continue; }
		const DirectX::BoundingBox* pLocalBox = GetLocalBoundingBox(entry.pModel);
		if (!pLocalBox) { continue; }

		DirectX::XMFLOAT3 corners[8] = {};
		pLocalBox->GetCorners(corners);
		const Math::Matrix localToLight = entry.pObj->GetMatrix() * a_lightView;
		for (const DirectX::XMFLOAT3& corner : corners)
		{
			const Math::Vector3 lightSpace = Math::Vector3::Transform(
				Math::Vector3(corner.x, corner.y, corner.z), localToLight);
			bounds.min = Math::Vector3::Min(bounds.min, lightSpace);
			bounds.max = Math::Vector3::Max(bounds.max, lightSpace);
		}
	}

	return bounds;
}

//=============================================================
// TryGetWorldBoundingSphere [private]
// モデルを持たない/バウンディングを計算できない場合はfalseを返し、
// 呼び出し側では「判定できない=常に描画する」扱いにする
//=============================================================
bool KdDrawPassRenderer::TryGetWorldBoundingSphere(const KdGameObject& a_obj, DirectX::BoundingSphere& a_outSphere)
{
	const DirectX::BoundingSphere* pLocalSphere = GetLocalBoundingSphere(a_obj.GetRenderModelData());
	if (!pLocalSphere) { return false; }

	pLocalSphere->Transform(a_outSphere, a_obj.GetMatrix());
	return true;
}

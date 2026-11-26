#include "Application/Effect/Particle/Gpu/ParticleGpuPool.h"
#include "Application/Shader/KdComputeShader.h"

// ビルド時に fxc がコンパイルしたバイナリをインクルードする
#include "Application/Shader/ParticleShader/Particle/ParticleEmit_CS.shaderInc"
#include "Application/Shader/ParticleShader/Particle/ParticleEmitReal_CS.shaderInc"
#include "Application/Shader/ParticleShader/Particle/ParticleUpdate_CS.shaderInc"
#include "Application/Shader/ParticleShader/Particle/BeamGpuBuild_CS.shaderInc"
#include "Application/Shader/ParticleShader/Ribbon/RibbonTrailUpdate_CS.shaderInc"
#include "Application/Shader/ParticleShader/Ribbon/RibbonGpuBuild_CS.shaderInc"
#include "Application/Shader/ParticleShader/ParticleShaderSet.h"

namespace Particle
{
	//=============================================================
	// CreatePoolBufs  [private]
	// パーティクルのプール本体・生存数カウンタ・free list を作り、隠しカウンタを初期化する。
	// 失敗した場合は作りかけのものを解放してfalseを返す
	//=============================================================
	bool ParticleGpuPool::CreatePoolBufs(UINT a_capacity)
	{
		if (!CreatePoolMainBufs(a_capacity))
		{
			Release();
			return false;
		}

		// 生存パーティクル数カウンタ(Emit CSでIncrement、Update CSでDecrement)
		if (!KdComputeShader::CreateCounterUav(
			{ sizeof(UINT), 1 },
			m_pAliveCounterBuf.GetAddressOf(), m_pAliveCounterUav.GetAddressOf()))
		{
			Release();
			return false;
		}

		if (!CreateAliveCntReadbackBufs())
		{
			Release();
			return false;
		}

		// free list: 初期値として 0..capacity-1 のインデックスを敷き詰める
		std::vector<UINT> initialIndices(a_capacity);
		for (UINT i = 0; i < a_capacity; ++i)
		{
			initialIndices[i] = i;
		}

		if (!KdComputeShader::CreateStructuredUav(
			{ sizeof(UINT), a_capacity, true },
			m_pFreeListBuf.GetAddressOf(), m_pFreeListUav.GetAddressOf(),
			initialIndices.data()))
		{
			Release();
			return false;
		}

		// Append/Consume・Counterの隠しカウンタは初期値が不定なため、
		// バインド時に明示的にリセットする(Dispatchは不要)
		// free list … 全スロットが空いている状態(capacity個ぶん)
		// alive counter … 生存数0
		{
			auto* pCtx = KdDirect3D::Instance().WorkDevContext();
			const UINT zero = 0;

			pCtx->CSSetUnorderedAccessViews(1, 1, m_pFreeListUav.GetAddressOf(), &a_capacity);
			pCtx->CSSetUnorderedAccessViews(2, 1, m_pAliveCounterUav.GetAddressOf(), &zero);

			ID3D11UnorderedAccessView* pNullUAVs[2] = { nullptr, nullptr };
			const UINT keepCnts[2] = { static_cast<UINT>(-1), static_cast<UINT>(-1) };
			pCtx->CSSetUnorderedAccessViews(1, 2, pNullUAVs, keepCnts);
		}

		return true;
	}

	//=============================================================
	// CreatePoolMainBufs
	// パーティクルプール本体のUAVと、描画時にVSから読むSRVを作る。失敗したらfalse(解放は呼び出し側)
	//=============================================================
	bool ParticleGpuPool::CreatePoolMainBufs(UINT a_capacity)
	{
		// パーティクルプール本体
		// 初期値は全要素ゼロ埋め(life=0)にする。DEFAULTバッファは初期値を
		// 指定しない場合の内容が不定のため、Draw()でcapacity全件を描画する際に
		// 未使用スロットのゴミデータ(life>0や NaN 等)が描画されるのを防ぐ
		{
			std::vector<ParticleData> zeroInit(a_capacity);

			if (!KdComputeShader::CreateStructuredUav(
				{ sizeof(ParticleData), a_capacity, false },
				m_pPoolBuf.GetAddressOf(), m_pPoolUav.GetAddressOf(),
				zeroInit.data()))
			{
				return false;
			}
		}

		// 描画時にVSから読むためのSRV(ParticleShader::Beginへ直接渡す)
		{
			auto* pDev = KdDirect3D::Instance().WorkDev();

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srvDesc.Buffer.FirstElement = 0;
			srvDesc.Buffer.NumElements = a_capacity;

			if (FAILED(pDev->CreateShaderResourceView(m_pPoolBuf.Get(), &srvDesc, m_pPoolSrv.GetAddressOf())))
			{
				return false;
			}
		}

		return true;
	}

	//=============================================================
	// CreateAliveCntReadbackBufs
	// 生存数の読み戻し用(CopyStructureCountの転送先とSTAGING)のバッファを作る。失敗したらfalse(解放は呼び出し側)
	//=============================================================
	bool ParticleGpuPool::CreateAliveCntReadbackBufs()
	{
		// GetAliveCntApprox() の CopyStructureCount 転送先
		{
			auto* pDev = KdDirect3D::Instance().WorkDev();

			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = sizeof(UINT);
			bd.Usage = D3D11_USAGE_DEFAULT;
			bd.BindFlags = 0;

			if (FAILED(pDev->CreateBuffer(&bd, nullptr, m_pCntReadbackBuf.GetAddressOf())))
			{
				return false;
			}

			// GetAliveCntDeferred() の転送先。CPUから直接読めるSTAGINGにしておく
			D3D11_BUFFER_DESC stagingDesc = {};
			stagingDesc.ByteWidth = sizeof(UINT);
			stagingDesc.Usage = D3D11_USAGE_STAGING;
			stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

			for (Microsoft::WRL::ComPtr<ID3D11Buffer>& pStagingBuf : m_pAliveStagingBufs)
			{
				if (FAILED(pDev->CreateBuffer(&stagingDesc, nullptr, pStagingBuf.GetAddressOf())))
				{
					return false;
				}
			}
		}

		return true;
	}

	//=============================================================
	// CreateComputeShaders  [private]
	// 放出・更新・ビーム/リボン生成のコンピュートシェーダーを作る
	//=============================================================
	bool ParticleGpuPool::CreateComputeShaders()
	{
		auto* pDev = KdDirect3D::Instance().WorkDev();

		struct ShaderSlot
		{
			const void*          pByteCode;
			SIZE_T               byteCodeSize;
			ID3D11ComputeShader** ppShader;
		};

		const ShaderSlot slots[] =
		{
			{ g_CS_ParticleEmit,        sizeof(g_CS_ParticleEmit),        m_pEmitCs.GetAddressOf() },
			{ g_CS_ParticleEmitReal,    sizeof(g_CS_ParticleEmitReal),    m_pEmitRealCs.GetAddressOf() },
			{ g_CS_ParticleUpdate,      sizeof(g_CS_ParticleUpdate),      m_pUpdateCs.GetAddressOf() },
			{ g_CS_BeamGpuBuild,        sizeof(g_CS_BeamGpuBuild),        m_pBeamBuildCs.GetAddressOf() },
			{ g_CS_RibbonTrailUpdate,   sizeof(g_CS_RibbonTrailUpdate),   m_pRibbonTrailUpdateCs.GetAddressOf() },
			{ g_CS_RibbonGpuBuild,      sizeof(g_CS_RibbonGpuBuild),      m_pRibbonBuildCs.GetAddressOf() },
		};

		for (const ShaderSlot& slot : slots)
		{
			if (FAILED(pDev->CreateComputeShader(slot.pByteCode, slot.byteCodeSize, nullptr, slot.ppShader)))
			{
				return false;
			}
		}

		return true;
	}

	//=============================================================
	// CreateConstantBufs  [private]
	// 各パスの定数バッファを作る
	//=============================================================
	bool ParticleGpuPool::CreateConstantBufs()
	{
		return m_cbEmit.Create()
			&& m_cbEmitInit.Create()
			&& m_cbUpdate.Create()
			&& m_cbBeamBuild.Create()
			&& m_cbRibbonTrailUpdate.Create()
			&& m_cbRibbonBuild.Create();
	}

	bool ParticleGpuPool::Init(UINT a_capacity)
	{
		Release();

		if (a_capacity == 0)
		{
			return false;
		}

		if (!CreatePoolBufs(a_capacity))
		{
			return false;
		}

		if (!CreateComputeShaders() || !CreateConstantBufs())
		{
			Release();
			return false;
		}

		m_capacity = a_capacity;

		return true;
	}

	void ParticleGpuPool::Release()
	{
		m_pEmitCs.Reset();
		m_pEmitRealCs.Reset();
		m_pUpdateCs.Reset();
		m_pBeamBuildCs.Reset();
		m_pRibbonTrailUpdateCs.Reset();
		m_pRibbonBuildCs.Reset();
		m_pCntReadbackBuf.Reset();

		for (int i = 0; i < ALIVE_STAGING_SLOT_CNT; ++i)
		{
			m_pAliveStagingBufs[i].Reset();
			m_isAliveStagingPending[i] = false;
		}

		m_aliveStagingWriteIdx = 0;
		m_aliveCntDeferred = 0;
		m_pAliveCounterUav.Reset();
		m_pAliveCounterBuf.Reset();
		m_pFreeListUav.Reset();
		m_pFreeListBuf.Reset();
		m_pPoolSrv.Reset();
		m_pPoolUav.Reset();
		m_pPoolBuf.Reset();
		m_pBeamVertexSrv.Reset();
		m_pBeamVertexUav.Reset();
		m_pBeamVertexBuf.Reset();
		m_pRibbonPointsUav.Reset();
		m_pRibbonPointsBuf.Reset();
		m_pRibbonStateUav.Reset();
		m_pRibbonStateBuf.Reset();
		m_pRibbonVertexSrv.Reset();
		m_pRibbonVertexUav.Reset();
		m_pRibbonVertexBuf.Reset();

		m_cbEmit.Release();
		m_cbEmitInit.Release();
		m_cbUpdate.Release();
		m_cbBeamBuild.Release();
		m_cbRibbonTrailUpdate.Release();
		m_cbRibbonBuild.Release();

		m_capacity = 0;
		m_emitSeed = 0;
		m_beamSegmentCntAllocated = 0;
		m_isRibbonBufsReady = false;
	}

	void ParticleGpuPool::Emit(UINT a_count)
	{
		if (a_count == 0 || !m_pEmitCs)
		{
			return;
		}

		auto* pCtx = KdDirect3D::Instance().WorkDevContext();

		EmitCB& cb = m_cbEmit.Work();
		cb.emitCnt = a_count;
		m_cbEmit.Write();

		pCtx->CSSetShader(m_pEmitCs.Get(), nullptr, 0);
		pCtx->CSSetConstantBuffers(0, 1, m_cbEmit.GetAddress());

		ID3D11UnorderedAccessView* uavs[3] = { m_pPoolUav.Get(), m_pFreeListUav.Get(), m_pAliveCounterUav.Get() };
		// free list・alive counterとも前回までの状態を維持する(-1)
		const UINT keepCnts[3] = { static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1) };
		pCtx->CSSetUnorderedAccessViews(0, 3, uavs, keepCnts);

		pCtx->Dispatch(KdComputeShader::CalcGroupCnt(a_count, 64), 1, 1);

		ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
		pCtx->CSSetUnorderedAccessViews(0, 3, nullUAVs, keepCnts);
		pCtx->CSSetShader(nullptr, nullptr, 0);
	}

	//=============================================================
	// EmitFromParam
	// EmitterParam に基づいて count 個ぶんスロットを確保し初期化する
	//=============================================================
	void ParticleGpuPool::EmitFromParam(UINT a_count, const EmitterParam& a_param, const Math::Vector3& a_worldPos)
	{
		if (a_count == 0 || !m_pEmitRealCs)
		{
			return;
		}

		auto* pCtx = KdDirect3D::Instance().WorkDevContext();

		++m_emitSeed;

		EmitInitCB& cb = m_cbEmitInit.Work();
		cb.emitCnt = a_count;
		cb.shape = static_cast<UINT>(a_param.shape);
		cb.shapeRadius = a_param.shapeRadius;
		cb.randSeed = m_emitSeed;

		cb.shapeBox = a_param.shapeBox;
		cb.speedMin = a_param.speedMin;

		cb.speedMax = a_param.speedMax;
		cb.lifeMin = a_param.lifeMin;
		cb.lifeMax = a_param.lifeMax;
		cb.sizeStart = a_param.sizeStart;

		cb.sizeEnd = a_param.isSizeOverLifetimeEnabled ? a_param.sizeEnd : a_param.sizeStart;
		cb.rotSpeedMin = a_param.rotSpeedMin;
		cb.rotSpeedMax = a_param.rotSpeedMax;
		cb.emissiveIntensity = a_param.emissiveIntensity;

		cb.colorStart = a_param.colorStart;
		cb.colorEnd = a_param.colorEnd;

		cb.emitterWorldPos = a_worldPos;
		cb.forceGravity = a_param.forceGravity;

		m_cbEmitInit.Write();

		pCtx->CSSetShader(m_pEmitRealCs.Get(), nullptr, 0);
		pCtx->CSSetConstantBuffers(0, 1, m_cbEmitInit.GetAddress());

		ID3D11UnorderedAccessView* uavs[3] = { m_pPoolUav.Get(), m_pFreeListUav.Get(), m_pAliveCounterUav.Get() };
		const UINT keepCnts[3] = { static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1) };
		pCtx->CSSetUnorderedAccessViews(0, 3, uavs, keepCnts);

		pCtx->Dispatch(KdComputeShader::CalcGroupCnt(a_count, 64), 1, 1);

		ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
		pCtx->CSSetUnorderedAccessViews(0, 3, nullUAVs, keepCnts);
		pCtx->CSSetShader(nullptr, nullptr, 0);
	}

	void ParticleGpuPool::Update(float a_dt)
	{
		if (m_capacity == 0 || !m_pUpdateCs)
		{
			return;
		}

		auto* pCtx = KdDirect3D::Instance().WorkDevContext();

		UpdateCB& cb = m_cbUpdate.Work();
		cb.dt = a_dt;
		cb.capacity = m_capacity;
		cb.magnetFieldEnabled = m_isMagnetFieldEnabled ? 1 : 0;
		cb.magnetSrcCnt = static_cast<UINT>(m_magnetFieldSrcs.size());
		cb.magnetFieldStrength = m_magnetFieldStrength;
		cb.magnetFieldTwist = m_magnetFieldTwist;

		for (UINT i = 0; i < cb.magnetSrcCnt; ++i)
		{
			const auto& src = m_magnetFieldSrcs[i];
			cb.magnetSrcs[i] = { src.pos.x, src.pos.y, src.pos.z, src.radius };
		}

		cb.colEnabled = m_isColEnabled ? 1 : 0;
		cb.colPlaneCnt = static_cast<UINT>(m_colPlanes.size());
		cb.colBounciness = m_colBounciness;
		cb.colFriction = m_colFriction;

		for (UINT i = 0; i < cb.colPlaneCnt; ++i)
		{
			const auto& plane = m_colPlanes[i];
			cb.colPlanePositions[i] = { plane.pos.x, plane.pos.y, plane.pos.z, 0.0f };
			cb.colPlaneNormals[i] = { plane.normal.x, plane.normal.y, plane.normal.z, 0.0f };
		}

		m_cbUpdate.Write();

		pCtx->CSSetShader(m_pUpdateCs.Get(), nullptr, 0);
		pCtx->CSSetConstantBuffers(0, 1, m_cbUpdate.GetAddress());

		ID3D11UnorderedAccessView* uavs[3] = { m_pPoolUav.Get(), m_pFreeListUav.Get(), m_pAliveCounterUav.Get() };
		// free list(Append先頭位置)・alive counterとも前回までの状態を維持する(-1)
		const UINT keepCnts[3] = { static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1) };
		pCtx->CSSetUnorderedAccessViews(0, 3, uavs, keepCnts);

		pCtx->Dispatch(KdComputeShader::CalcGroupCnt(m_capacity, 64), 1, 1);

		ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
		pCtx->CSSetUnorderedAccessViews(0, 3, nullUAVs, keepCnts);
		pCtx->CSSetShader(nullptr, nullptr, 0);
	}

	//=============================================================
	// SetMagnetField
	// 磁力ベクトル場。次回以降のUpdate()で参照する値を保持する
	//=============================================================
	void ParticleGpuPool::SetMagnetField(bool a_isEnabled, float a_strength, float a_twist, const std::vector<MagnetFieldSource>& a_srcs)
	{
		m_isMagnetFieldEnabled = a_isEnabled;
		m_magnetFieldStrength = a_strength;
		m_magnetFieldTwist = a_twist;

		m_magnetFieldSrcs = a_srcs;

		if (m_magnetFieldSrcs.size() > static_cast<size_t>(MAGNET_FIELD_MAX_SOURCES))
		{
			m_magnetFieldSrcs.resize(MAGNET_FIELD_MAX_SOURCES);
		}
	}

	//=============================================================
	// SetColPlanes
	// コリジョン。次回以降のUpdate()で参照する値を保持する
	//=============================================================
	void ParticleGpuPool::SetColPlanes(bool a_isEnabled, float a_bounciness, float a_friction, const std::vector<CollisionPlane>& a_planes)
	{
		m_isColEnabled = a_isEnabled;
		m_colBounciness = a_bounciness;
		m_colFriction = a_friction;

		m_colPlanes = a_planes;

		if (m_colPlanes.size() > static_cast<size_t>(COLLISION_MAX_PLANES))
		{
			m_colPlanes.resize(COLLISION_MAX_PLANES);
		}
	}

	UINT ParticleGpuPool::GetAliveCntApprox()
	{
		if (!m_pAliveCounterUav || !m_pCntReadbackBuf)
		{
			return 0;
		}

		auto* pCtx = KdDirect3D::Instance().WorkDevContext();
		pCtx->CopyStructureCount(m_pCntReadbackBuf.Get(), 0, m_pAliveCounterUav.Get());

		UINT count = 0;

		if (!KdComputeShader::ReadBackBuf(m_pCntReadbackBuf.Get(), sizeof(UINT), &count))
		{
			return 0;
		}

		return count;
	}

	//=============================================================
	// GetAliveCntDeferred
	// 転送を積んでおき、GPUが書き終えたものだけを待たずに読む(DO_NOT_WAIT)。
	// 古い転送から順に読むため、返す値は常に新しくなる方向へ進む
	//=============================================================
	UINT ParticleGpuPool::GetAliveCntDeferred()
	{
		if (!m_pAliveCounterUav || !m_pAliveStagingBufs[0])
		{
			return m_aliveCntDeferred;
		}

		auto* pCtx = KdDirect3D::Instance().WorkDevContext();

		for (int i = 0; i < ALIVE_STAGING_SLOT_CNT; ++i)
		{
			const int index = (m_aliveStagingWriteIdx + i) % ALIVE_STAGING_SLOT_CNT;

			if (!m_isAliveStagingPending[index])
			{
				continue;
			}

			D3D11_MAPPED_SUBRESOURCE mapped = {};

			if (pCtx->Map(m_pAliveStagingBufs[index].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK)
			{
				continue;
			}

			m_aliveCntDeferred = *static_cast<const UINT*>(mapped.pData);
			pCtx->Unmap(m_pAliveStagingBufs[index].Get(), 0);
			m_isAliveStagingPending[index] = false;
		}

		// 書き込み先が空いていれば、次の読み出しを積む
		const int writeIdx = m_aliveStagingWriteIdx;

		if (!m_isAliveStagingPending[writeIdx])
		{
			pCtx->CopyStructureCount(m_pAliveStagingBufs[writeIdx].Get(), 0, m_pAliveCounterUav.Get());
			m_isAliveStagingPending[writeIdx] = true;
			m_aliveStagingWriteIdx = (writeIdx + 1) % ALIVE_STAGING_SLOT_CNT;
		}

		return m_aliveCntDeferred;
	}

	//=============================================================
	// Draw
	// 既存の ParticleShader をそのまま流用し、プールのSRVを直接バインドして描画する
	//
	// 注意: CPU経路(ParticleEffect)は生存パーティクルが常に配列の先頭から
	// 密に詰まっている前提だが、GPUプールは free list による使い回しのため
	// 生存スロットが capacity 全体に疎に散らばる。そのため aliveCnt 件だけ
	// ではなく capacity 全件を描画対象とし、死亡/未使用スロットは
	// Particle_VS.hlsl 側の life<=0 ガードで非表示にする
	//=============================================================
	void ParticleGpuPool::Draw(const DrawDesc& a_drawDesc)
	{
		const RenderMode renderMode = a_drawDesc.renderMode;
		const float stretchLenScale = a_drawDesc.stretchLenScale;
		const Math::Matrix& viewMat = a_drawDesc.viewMat;
		const KdTexture* pTex = a_drawDesc.pTex;
		const BlendMode blendMode = a_drawDesc.blendMode;

		if (!m_pPoolSrv || m_capacity == 0)
		{
			return;
		}

		// 生存数0でも早期returnしない。生存数の読み出しはGPUの完了を待つ同期になり、
		// 毎フレーム行うとCPUが待たされ、GPUにも空き時間ができる。
		// 死亡/未使用スロットはParticle_VS.hlsl側のlife<=0ガードで描かれない

		auto& particleShader = PARTICLE_SHADERS.GetParticleShader();

		particleShader.Begin(blendMode, m_pPoolSrv.Get());
		particleShader.SetTex(pTex);
		ParticleShader::EmitterDrawDesc desc;
		desc.viewMat = viewMat;
		desc.aliveCnt = static_cast<int>(m_capacity);
		desc.pTex = pTex;
		desc.renderMode = renderMode;
		desc.stretchLenScale = stretchLenScale;
		particleShader.WriteEmitterCb(desc);
		particleShader.DrawParticles(static_cast<int>(m_capacity));
		particleShader.End();
	}

	//=============================================================
	// EnsureBeamBuf
	// ビーム頂点バッファを capacity * (segmentCnt+1) 要素で確保する。
	// 要求segmentCountが前回と同じなら何もしない
	//=============================================================
	bool ParticleGpuPool::EnsureBeamBuf(UINT a_segmentCnt)
	{
		a_segmentCnt = std::max(1u, a_segmentCnt);

		if (m_pBeamVertexBuf && m_beamSegmentCntAllocated == a_segmentCnt)
		{
			return true;
		}

		m_pBeamVertexSrv.Reset();
		m_pBeamVertexUav.Reset();
		m_pBeamVertexBuf.Reset();
		m_beamSegmentCntAllocated = 0;

		const UINT stride = a_segmentCnt + 1;
		const UINT elementCnt = m_capacity * stride;

		if (!KdComputeShader::CreateStructuredUav(
			{ sizeof(RibbonVertexData), elementCnt, false },
			m_pBeamVertexBuf.GetAddressOf(), m_pBeamVertexUav.GetAddressOf()))
		{
			return false;
		}

		auto* pDev = KdDirect3D::Instance().WorkDev();

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = elementCnt;

		if (FAILED(pDev->CreateShaderResourceView(m_pBeamVertexBuf.Get(), &srvDesc, m_pBeamVertexSrv.GetAddressOf())))
		{
			m_pBeamVertexUav.Reset();
			m_pBeamVertexBuf.Reset();
			return false;
		}

		m_beamSegmentCntAllocated = a_segmentCnt;

		return true;
	}

	//=============================================================
	// DrawBeam
	// BeamGpuBuild_CS.hlslでプール全パーティクルからビーム頂点列を構築し、
	// RibbonShaderのSRVオーバーライド機能でそのまま描画する
	//=============================================================
	void ParticleGpuPool::DrawBeam(const DrawDesc& a_desc, const EmitterParam& a_param, const Math::Matrix& a_emitterWorldMat)
	{
		const Math::Matrix& viewMat = a_desc.viewMat;
		const KdTexture* pTex = a_desc.pTex;
		const BlendMode blendMode = a_desc.blendMode;
		const float time = a_desc.time;

		if (m_capacity == 0 || !m_pBeamBuildCs || !m_pPoolUav)
		{
			return;
		}

		const UINT segmentCnt = static_cast<UINT>(std::max(1, a_param.beamSegmentCnt));

		if (!EnsureBeamBuf(segmentCnt))
		{
			return;
		}

		// 生存数0でも早期returnしない(理由はDraw()と同じ)。
		// 死亡スロットはBeamGpuBuild_CS.hlsl側で透明の頂点になる

		auto* pCtx = KdDirect3D::Instance().WorkDevContext();

		// FixedOffsetモードのローカル方向をワールド回転で変換しておく
		// (CPU版ParticleEffect::DrawBeamと同じ。回転成分のみでよいのでTransformNormal)
		const Math::Vector3 fixedOffsetWorld =
			Math::Vector3::TransformNormal(a_param.beamTargetOffset, a_emitterWorldMat);

		// 速度がほぼ0の粒子のフォールバック方向(エミッターのローカルZ+をワールドへ変換)
		const Math::Vector3 fallbackDir =
			Math::Vector3::TransformNormal({ 0.0f, 0.0f, 1.0f }, a_emitterWorldMat);

		BeamBuildCB& cb = m_cbBeamBuild.Work();
		cb.capacity = m_capacity;
		cb.segmentCnt = segmentCnt;
		cb.dirMode = static_cast<UINT>(a_param.beamDirMode);
		cb.beamLen = a_param.beamLen;
		cb.fixedOffsetWorld = fixedOffsetWorld;
		cb.noiseAmplitude = a_param.beamNoiseAmplitude;
		cb.fallbackDir = fallbackDir;
		cb.noiseFreq = a_param.beamNoiseFreq;
		cb.time = time;
		cb.ribbonWidthStart = a_param.ribbonWidthStart;
		cb.ribbonWidthEnd = a_param.ribbonWidthEnd;
		m_cbBeamBuild.Write();

		pCtx->CSSetShader(m_pBeamBuildCs.Get(), nullptr, 0);
		pCtx->CSSetConstantBuffers(0, 1, m_cbBeamBuild.GetAddress());

		ID3D11UnorderedAccessView* uavs[2] = { m_pPoolUav.Get(), m_pBeamVertexUav.Get() };
		const UINT keepCnts[2] = { static_cast<UINT>(-1), static_cast<UINT>(-1) };
		pCtx->CSSetUnorderedAccessViews(0, 2, uavs, keepCnts);

		pCtx->Dispatch(KdComputeShader::CalcGroupCnt(m_capacity, 64), 1, 1);

		ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
		pCtx->CSSetUnorderedAccessViews(0, 2, nullUAVs, keepCnts);
		pCtx->CSSetShader(nullptr, nullptr, 0);

		// 構築した頂点列をRibbonShaderでそのまま描画する
		// (groupId=パーティクルスロット番号のため、異なる粒子同士の帯は
		//  Ribbon_VS.hlsl側のgroupId不一致判定で自動的に繋がらない)
		const UINT stride = segmentCnt + 1;
		const int totalPoints = static_cast<int>(m_capacity * stride);

		auto& ribbonShader = PARTICLE_SHADERS.GetRibbonShader();

		ribbonShader.Begin(blendMode, m_pBeamVertexSrv.Get());
		ribbonShader.SetTex(pTex);
		ribbonShader.WriteEmitterCb(viewMat, totalPoints, a_param.ribbonUvMode, time * a_param.ribbonUvScrollSpeed);
		ribbonShader.DrawSegments(totalPoints - 1);
		ribbonShader.End();
	}

	//=============================================================
	// EnsureRibbonBufs
	// リボン(トレイル)用のバッファ群を初回のみ確保する
	// (capacity * RIBBON_GPU_MAX_POINTS で固定サイズのため、Beamと違い
	//  一度作れば作り直す必要はない)
	//=============================================================
	bool ParticleGpuPool::EnsureRibbonBufs()
	{
		if (m_isRibbonBufsReady)
		{
			return true;
		}

		const UINT pointElementCnt = m_capacity * static_cast<UINT>(RIBBON_GPU_MAX_POINTS);

		if (!KdComputeShader::CreateStructuredUav(
			{ sizeof(RibbonTrailPointData), pointElementCnt, false },
			m_pRibbonPointsBuf.GetAddressOf(), m_pRibbonPointsUav.GetAddressOf()))
		{
			return false;
		}

		std::vector<RibbonTrailState> zeroStates(m_capacity);

		if (!KdComputeShader::CreateStructuredUav(
			{ sizeof(RibbonTrailState), m_capacity, false },
			m_pRibbonStateBuf.GetAddressOf(), m_pRibbonStateUav.GetAddressOf(),
			zeroStates.data()))
		{
			return false;
		}

		if (!KdComputeShader::CreateStructuredUav(
			{ sizeof(RibbonVertexData), pointElementCnt, false },
			m_pRibbonVertexBuf.GetAddressOf(), m_pRibbonVertexUav.GetAddressOf()))
		{
			return false;
		}

		auto* pDev = KdDirect3D::Instance().WorkDev();

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = pointElementCnt;

		if (FAILED(pDev->CreateShaderResourceView(m_pRibbonVertexBuf.Get(), &srvDesc, m_pRibbonVertexSrv.GetAddressOf())))
		{
			return false;
		}

		m_isRibbonBufsReady = true;

		return true;
	}

	void ParticleGpuPool::UpdateRibbonTrail(float a_dt, const EmitterParam& a_param)
	{
		if (m_capacity == 0 || !m_pRibbonTrailUpdateCs || !m_pPoolUav)
		{
			return;
		}

		if (!EnsureRibbonBufs())
		{
			return;
		}

		auto* pCtx = KdDirect3D::Instance().WorkDevContext();

		RibbonTrailUpdateCB& cb = m_cbRibbonTrailUpdate.Work();
		cb.capacity = m_capacity;
		cb.dt = a_dt;
		cb.ribbonLifeTime = a_param.ribbonLifeTime;
		cb.ribbonMinPointDist = a_param.ribbonMinPointDist;
		m_cbRibbonTrailUpdate.Write();

		pCtx->CSSetShader(m_pRibbonTrailUpdateCs.Get(), nullptr, 0);
		pCtx->CSSetConstantBuffers(0, 1, m_cbRibbonTrailUpdate.GetAddress());

		ID3D11UnorderedAccessView* uavs[3] = { m_pPoolUav.Get(), m_pRibbonPointsUav.Get(), m_pRibbonStateUav.Get() };
		const UINT keepCnts[3] = { static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1) };
		pCtx->CSSetUnorderedAccessViews(0, 3, uavs, keepCnts);

		pCtx->Dispatch(KdComputeShader::CalcGroupCnt(m_capacity, 64), 1, 1);

		ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
		pCtx->CSSetUnorderedAccessViews(0, 3, nullUAVs, keepCnts);
		pCtx->CSSetShader(nullptr, nullptr, 0);
	}

	//=============================================================
	// DrawRibbon
	// RibbonGpuBuild_CS.hlslでUpdateRibbonTrail()が積み上げた履歴を
	// RibbonVertexDataへ変換し、RibbonShaderのSRVオーバーライド機能で
	// そのまま描画する
	//=============================================================
	void ParticleGpuPool::DrawRibbon(const DrawDesc& a_desc, const EmitterParam& a_param)
	{
		const Math::Matrix& viewMat = a_desc.viewMat;
		const KdTexture* pTex = a_desc.pTex;
		const BlendMode blendMode = a_desc.blendMode;
		const float time = a_desc.time;

		if (m_capacity == 0 || !m_pRibbonBuildCs || !m_isRibbonBufsReady)
		{
			return;
		}

		// 生存数0でも早期returnしない: 死亡後も既存のトレイル点は
		// ribbonLifeTimeぶん尾を引きながらフェードするため、最後の1粒が
		// 死んだ直後もしばらく描画対象が残る(GetAliveCntApprox()は
		// 「生きている粒子」のみを数えるため、この判定には使えない)

		auto* pCtx = KdDirect3D::Instance().WorkDevContext();

		RibbonBuildCB& cb = m_cbRibbonBuild.Work();
		cb.capacity = m_capacity;
		cb.ribbonLifeTime = a_param.ribbonLifeTime;
		cb.ribbonWidthStart = a_param.ribbonWidthStart;
		cb.ribbonWidthEnd = a_param.ribbonWidthEnd;
		m_cbRibbonBuild.Write();

		pCtx->CSSetShader(m_pRibbonBuildCs.Get(), nullptr, 0);
		pCtx->CSSetConstantBuffers(0, 1, m_cbRibbonBuild.GetAddress());

		ID3D11UnorderedAccessView* uavs[4] = { m_pPoolUav.Get(), m_pRibbonPointsUav.Get(), m_pRibbonStateUav.Get(), m_pRibbonVertexUav.Get() };
		const UINT keepCnts[4] = { static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1) };
		pCtx->CSSetUnorderedAccessViews(0, 4, uavs, keepCnts);

		pCtx->Dispatch(KdComputeShader::CalcGroupCnt(m_capacity, 64), 1, 1);

		ID3D11UnorderedAccessView* nullUAVs[4] = { nullptr, nullptr, nullptr, nullptr };
		pCtx->CSSetUnorderedAccessViews(0, 4, nullUAVs, keepCnts);
		pCtx->CSSetShader(nullptr, nullptr, 0);

		// 固定長(capacity * RIBBON_GPU_MAX_POINTS)で構築しているため、
		// 未使用区画は幅0の無効頂点として埋まっている(BeamGpuBuild_CS/DrawBeamと違い
		// パーティクルごとに有効点数が変わるため、全区画を常に描画対象にする)
		const int totalPoints = static_cast<int>(m_capacity) * RIBBON_GPU_MAX_POINTS;

		auto& ribbonShader = PARTICLE_SHADERS.GetRibbonShader();

		ribbonShader.Begin(blendMode, m_pRibbonVertexSrv.Get());
		ribbonShader.SetTex(pTex);
		ribbonShader.WriteEmitterCb(viewMat, totalPoints, a_param.ribbonUvMode, time * a_param.ribbonUvScrollSpeed);
		ribbonShader.DrawSegments(totalPoints - 1);
		ribbonShader.End();
	}

}	// namespace Particle

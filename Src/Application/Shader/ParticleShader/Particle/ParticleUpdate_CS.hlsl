// このファイルはBaseFramework(提供テンプレート)には存在しない、独自追加ファイルです。

// ParticleUpdate_CS.hlsl
// GPUパーティクル 更新用コンピュートシェーダー
//
// プール全スロットを走査し、生存中(life > 0)のパーティクルのみ
// 位置を積分し寿命を減算する。寿命が尽きたスロットは
// free list(AppendStructuredBuffer)へ返却する。
//
// 現時点ではプール全体(capacity件)を毎回走査する素朴な実装。
// 生存パーティクルのみを効率よく辿る仕組み(コンパクトな生存リスト等)は
// 必要になった時点で最適化として追加する

#include "../ParticleGpuLayout.hlsli"

RWStructuredBuffer<ParticleData> g_Pool : register(u0);
AppendStructuredBuffer<uint> g_FreeList : register(u1);
RWStructuredBuffer<uint> g_AliveCounter : register(u2);

// 磁力ベクトル場ソース1件。xyz=ワールド座標、w=半径(0以下は未使用)
#define MAGNET_FIELD_MAX_SOURCES 4

// コリジョンの平面数上限
#define COLLISION_MAX_PLANES 4

cbuffer cbUpdate : register(b0)
{
	float g_DeltaTime;
	uint g_Capacity;
	uint g_MagnetFieldEnabled;
	uint g_MagnetSourceCount;

	float g_MagnetFieldStrength;
	float g_MagnetFieldTwist;
	float2 g_Pad;

	float4 g_MagnetSources[MAGNET_FIELD_MAX_SOURCES];

	uint  g_CollisionEnabled;
	uint  g_CollisionPlaneCount;
	float g_CollisionBounciness;
	float g_CollisionFriction;

	float4 g_CollisionPlanePositions[COLLISION_MAX_PLANES];
	float4 g_CollisionPlaneNormals[COLLISION_MAX_PLANES];
};

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint idx = dispatchThreadId.x;

	if (idx >= g_Capacity)
	{
		return;
	}

	ParticleData p = g_Pool[idx];

	// 未使用 / 既に死んでいるスロットは触らない
	if (p.life <= 0.0f)
	{
		return;
	}

	p.life -= g_DeltaTime;
	p.velocity += p.gravity * g_DeltaTime;

	// 磁力ベクトル場。ソースごとに向心方向+接線方向(渦)の加速度を合成して積分する
	// 減衰は線形(distが0でstrength/twist最大、radius以遠で0)。CPU版Simulate()と同じ計算式
	if (g_MagnetFieldEnabled != 0)
	{
		for (uint i = 0; i < g_MagnetSourceCount; ++i)
		{
			float3 sourcePos = g_MagnetSources[i].xyz;
			float radius = g_MagnetSources[i].w;

			if (radius <= 0.0f)
			{
				continue;
			}

			float3 toSource = sourcePos - p.position;
			float dist = length(toSource);

			if (dist >= radius || dist <= 1e-4f)
			{
				continue;
			}

			float t = 1.0f - dist / radius;
			float3 radial = toSource / dist;

			float3 tangent = cross(radial, float3(0.0f, 1.0f, 0.0f));
			float tangentLenSqr = dot(tangent, tangent);
			if (tangentLenSqr > 1e-6f)
			{
				tangent *= rsqrt(tangentLenSqr);
			}

			p.velocity += (radial * g_MagnetFieldStrength + tangent * g_MagnetFieldTwist) * t * g_DeltaTime;
		}
	}

	p.position += p.velocity * g_DeltaTime;

	// コリジョン(平面コリジョン)。めり込んだ粒子を法線方向へ反射させる。CPU版Simulate()と同じ計算式
	if (g_CollisionEnabled != 0)
	{
		for (uint pi = 0; pi < g_CollisionPlaneCount; ++pi)
		{
			float3 planePos = g_CollisionPlanePositions[pi].xyz;
			float3 planeNormal = g_CollisionPlaneNormals[pi].xyz;

			float signedDist = dot(p.position - planePos, planeNormal);

			if (signedDist >= 0.0f)
			{
				continue;
			}

			float velAlongNormal = dot(p.velocity, planeNormal);

			if (velAlongNormal >= 0.0f)
			{
				continue;
			}

			p.position -= planeNormal * signedDist;

			float3 velNormal = planeNormal * velAlongNormal;
			float3 velTangent = p.velocity - velNormal;

			p.velocity = velTangent * (1.0f - g_CollisionFriction) - velNormal * g_CollisionBounciness;
		}
	}

	if (p.life <= 0.0f)
	{
		p.life = 0.0f;
		g_FreeList.Append(idx);
		g_AliveCounter.DecrementCounter();
	}

	g_Pool[idx] = p;
}

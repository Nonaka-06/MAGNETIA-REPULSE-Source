// このファイルはBaseFramework(提供テンプレート)には存在しない、独自追加ファイルです。

// ParticleEmit_CS.hlsl
// GPUパーティクル 放出用コンピュートシェーダー
//
// free list(ConsumeStructuredBuffer)から空きスロットを確保し、
// パーティクルプール(RWStructuredBuffer)へ初期化データを書き込む
//
// 現時点では動作確認用の仮の初期化(原点から円状に広がって上昇する
// 噴水状のパターン)を行う。エミッターパラメータに基づく本来の初期化
// (EmitterNodeの位置・形状・カーブ等の反映)は今後のステップで実装する。
//
// rotationSpeed に確保したスロット番号を書き込んでいるのは
// free listの重複/欠落が無いことをCPU側のセルフテストで検証するための
// マーカーで、見た目にはほぼ影響しない(板ポリのZ回転速度に使われるのみ)

#include "../ParticleGpuLayout.hlsli"

RWStructuredBuffer<ParticleData> g_Pool : register(u0);
ConsumeStructuredBuffer<uint> g_FreeList : register(u1);
RWStructuredBuffer<uint> g_AliveCounter : register(u2);

cbuffer cbEmit : register(b0)
{
	uint g_EmitCount;
	uint3 g_Pad;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x >= g_EmitCount)
	{
		return;
	}

	uint slot = g_FreeList.Consume();
	g_AliveCounter.IncrementCounter();

	ParticleData p = (ParticleData)0;
	p.life = 1.0f;
	p.lifeMax = 1.0f;

	// 動作確認用: 原点から円状に広がりながら緩やかに上昇する
	const float angle = (float) (slot % 32) * (6.2831853f / 32.0f);
	p.position = float3(0.0f, 0.0f, 0.0f);
	p.velocity = float3(cos(angle) * 0.6f, 1.5f, sin(angle) * 0.6f);

	p.colorStart = float4(1.0f, 1.0f, 1.0f, 1.0f);
	p.colorEnd = float4(1.0f, 1.0f, 1.0f, 0.0f);
	p.sizeStart = 0.4f;
	p.sizeEnd = 0.1f;

	// 動作確認用: 1.0を超えるHDR発光強度にし、ブルームが滲むかを目視確認できるようにする
	p.emissiveIntensity = 3.0f;

	// free listの重複/欠落検証用マーカー(セルフテスト専用。見た目にはほぼ影響しない)
	p.rotationSpeed = (float) slot;

	g_Pool[slot] = p;
}

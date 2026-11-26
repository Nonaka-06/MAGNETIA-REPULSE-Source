// このファイルはBaseFramework(提供テンプレート)には存在しない、独自追加ファイルです。

// Particle_VS.hlsl
// 2D板ポリGPUパーティクル 頂点シェーダー
//
// StructuredBuffer から ParticleData を読み込み
// ビルボード行列 + スケール + Z回転で板ポリを配置する
// テクスチャのアスペクト比（幅/高さ）を cbEmitter から受け取り
// 板ポリの X 方向オフセットに掛けることで引き延ばしを防ぐ

#include "../../../../Framework/Shader/inc_KdCommon.hlsli"
#include "../ParticleGpuLayout.hlsli"

//--------------------------------------------------------------
// StructuredBuffer（スロット t5 にバインド）
//--------------------------------------------------------------
StructuredBuffer<ParticleData> g_particles : register(t5);

//--------------------------------------------------------------
// エミッター定数バッファ（スロット b4）
//
// 並びは ParticleConst.h の cbEmitter と同じ。VSで使わないメンバ(ディゾルブ・グリッチ)も
// 並びを揃えるため宣言だけ持つ
//--------------------------------------------------------------
cbuffer cbEmitter : register(b4)
{
	row_major float4x4 g_mEview;
	int g_aliveCount;
	float g_texAspect; // テクスチャのアスペクト比 (幅 / 高さ)
	int g_renderMode;
	float g_stretchLengthScale;

	int g_dissolveEnabled;
	float g_dissolveEdgeWidth;
	float g_dissolveEdgeIntensity;
	float g_dissolveNoiseScale;
	float4 g_dissolveEdgeColor;

	int g_glitchEnabled;
	float g_glitchBlockScale;
	float g_glitchIntensity;
	float g_glitchSpeed;
	float g_glitchColorSplit;
	float g_glitchTime;
	float2 g_pad_glitch;

	int g_sizeUseCurve;
	int g_stretchWorldAligned; // 旧pad3[0]を転用。renderMode==Stretchの場合のみ使用(拡張)
	float2 g_pad3;
	float4 g_sizeCurveLUT[4]; // 16サンプル(float4×4)。CURVE_LUT_SAMPLES(ParticleConst.h)と対応

	int g_alphaUseCurve;
	float3 g_pad4;
	float4 g_alphaCurveLUT[4];

	int g_rotationUseCurve;
	float3 g_pad5;
	float4 g_rotationCurveLUT[4]; // ラジアン

	// StructuredBuffer上の読み出し開始位置。
	// 共有バッファへ1フレーム分を順に積むため、エミッターごとに起点が違う
	int g_particleStartIndex;
	int g_groundPlaneAligned;

	// フォグの掛け方(ParticleConst.h::FogMode)。0=受けない、1=色をフォグ色へ混ぜる、2=フォグの分だけ暗くする
	int g_fogMode;

	// 光を受ける割合(0〜1)。0なら受光の計算を丸ごと省く
	float g_lightResponse;
};

//--------------------------------------------------------------
// 16サンプルのLUT(float4×4)から寿命進行度t01(0〜1)に対応する値を線形補間でサンプルする
// (サイズ/アルファ/回転角カーブで共用)
//--------------------------------------------------------------
float SampleCurveLUT16(float4 lut[4], float t01)
{
	const float scaled = saturate(t01) * 15.0f; // 16サンプル → インデックス0〜15
	// 0〜15で負にならないため、符号なしにして配列番号の割り算を軽くする
	const uint i0 = (uint) floor(scaled);
	const uint i1 = min(i0 + 1u, 15u);
	const float frac_ = scaled - (float) i0;

	const float v0 = lut[i0 / 4][i0 % 4];
	const float v1 = lut[i1 / 4][i1 % 4];

	return lerp(v0, v1, frac_);
}

//--------------------------------------------------------------
// 板ポリ4頂点のローカルオフセット
// TRIANGLESTRIP 順：左下・左上・右下・右上
// X 成分はアスペクト比補正前の値。シェーダー内で g_texAspect を乗算する
//--------------------------------------------------------------
// 6頂点で1Quad（TRIANGLELIST）
// 頂点インデックス → Quadローカル頂点の対応
//   0→左下, 1→左上, 2→右下,   (三角形1)
//   3→左上, 4→右上, 5→右下    (三角形2)
static const float2 kQuadOffset[6] =
{
	float2(-0.5f, -0.5f), // 0 左下
    float2(-0.5f, 0.5f), // 1 左上
    float2(0.5f, -0.5f), // 2 右下
    float2(-0.5f, 0.5f), // 3 左上 (再利用)
    float2(0.5f, 0.5f), // 4 右上
    float2(0.5f, -0.5f), // 5 右下 (再利用)
};

static const float2 kQuadUV[6] =
{
	float2(0.0f, 1.0f), // 0
    float2(0.0f, 0.0f), // 1
    float2(1.0f, 1.0f), // 2
    float2(0.0f, 0.0f), // 3
    float2(1.0f, 0.0f), // 4
    float2(1.0f, 1.0f), // 5
};

//--------------------------------------------------------------
// 受光
// 粒子は法線を持たないため、板を全方向から光を受ける面として扱い、
// 光の強さへ一律の係数を掛ける(標準シェーダーの Lambert は法線との内積と 1/π を掛けるが、
// 内積は平均すれば半分ほどで、粒子は明るく見せたいため 0.5 に揃える)。
// 減衰は標準シェーダーの点光源と同じ (1 - 距離/半径)^2 で、半径の外は即座に飛ばす。
// 影は見ない(頂点シェーダーでは影の判定を持てないため、建物の陰の煙にも光が当たる)
//--------------------------------------------------------------
static const float PARTICLE_LIGHT_GAIN = 0.5f;

// 点光源の登録数の上限(cbLight の配列の大きさと同じ)
static const int PARTICLE_POINT_LIGHT_MAX = 100;

// 街灯(有限長ライト)の登録数の上限(cbLight の配列の大きさと同じ)
static const int PARTICLE_TUBE_LIGHT_MAX = 32;

// 受けた光の合計の上限。点光源は色の大きさがそのまま強さのため、炎のすぐ横の煙が
// ブルームで白飛びしないように抑える上限
static const float PARTICLE_LIGHT_SUM_MAX = 3.0f;

float3 ApplyParticleLighting(float3 worldPos, float3 rgb)
{
	if (g_lightResponse <= 0.0f)
	{
		return rgb;
	}

	float3 lightSum = g_AmbientLight.rgb + g_DL_Color * PARTICLE_LIGHT_GAIN;

	const int pointCount = min(g_PointLightNum.x, PARTICLE_POINT_LIGHT_MAX);

	[loop]
	for (int i = 0; i < pointCount; ++i)
	{
		const float dist = length(g_PointLights[i].Pos - worldPos);

		if (dist >= g_PointLights[i].Radius)
		{
			continue;
		}

		float atte = 1.0f - saturate(dist / g_PointLights[i].Radius);
		atte *= atte;

		lightSum += g_PointLights[i].Color * (atte * PARTICLE_LIGHT_GAIN);
	}

	// 街灯は配光の円錐を含めた影響度(0〜1)を使い、光の中にだけ粒子が浮かび上がるようにする
	const int tubeCount = min(g_TubeLightNum.x, PARTICLE_TUBE_LIGHT_MAX);

	[loop]
	for (int t = 0; t < tubeCount; ++t)
	{
		const float influence = KdEvaluateTubeLightInfluence(g_TubeLights[t], worldPos);

		lightSum += g_TubeLights[t].Color * (g_TubeLights[t].Intensity * influence * PARTICLE_LIGHT_GAIN);
	}

	lightSum = min(lightSum, PARTICLE_LIGHT_SUM_MAX);

	return lerp(rgb, rgb * lightSum, saturate(g_lightResponse));
}

//--------------------------------------------------------------
// フォグ
// KdStandardShader_PS_Lit.hlsl の高さフォグ→距離フォグと同じ式を、粒子の中心位置で1回だけ評価する。
// 板ごとに画素単位で評価しないのは、Particle_PS.hlslをRibbon/EdgeTrailと共有しており、
// 入力を増やすとそれらのVS出力と合わなくなるため(粒子の板は小さく、中心の値で足りる)。
//
// 加算合成にフォグ色を足すと、遠いほど明るくなってしまう。
// そのため加算合成ではフォグ色へ混ぜず、フォグの分だけ暗くする(g_fogMode == 2)
//--------------------------------------------------------------
float3 ApplyParticleFog(float3 worldPos, float3 rgb)
{
	if (g_fogMode == 0)
	{
		return rgb;
	}

	const float camDist = length(worldPos - g_CamPos);

	// 通常アルファ合成用(フォグ色へ混ぜた色)と、加算合成用(元の色に掛ける残り)を同時に求める
	float3 tinted = rgb;

	// 1=フォグなし、0=フォグ色に埋まる
	float remain = 1.0f;

	// 式は inc_KdCommon.hlsli の KdHeightFogRate()。標準シェーダーと同じ濃さになる
	if (g_HeightFogEnable != 0)
	{
		const float fogRate = KdHeightFogRate(worldPos, camDist);

		tinted = lerp(tinted, g_HeightFogColor, fogRate);
		remain *= 1.0f - fogRate;
	}

	if (g_DistanceFogEnable != 0)
	{
		const float f = saturate(1.0f / exp(camDist * g_DistanceFogDensity));

		tinted = lerp(g_DistanceFogColor, tinted, f);
		remain *= f;
	}

	return (g_fogMode == 2) ? rgb * remain : tinted;
}

//--------------------------------------------------------------
// 出力構造体
//--------------------------------------------------------------
struct VSOut
{
	float4 svPos : SV_POSITION;
	float2 uv : TEXCOORD0;
	float4 color : COLOR;
	float dissolveT : TEXCOORD1; // 寿命進行度(0=生まれたて,1=消滅直前)。ディゾルブ用
};

//--------------------------------------------------------------
// メイン
// SV_VertexID から「パーティクルIndex」と「頂点Index(0-3)」を取得
//--------------------------------------------------------------
VSOut main(uint vertId : SV_VertexID)
{
	VSOut o = (VSOut) 0;

	 // 6頂点で1パーティクル
	const uint particleIdx = vertId / 6;
	const uint quadVtx = vertId % 6;

	if ((int) particleIdx >= g_aliveCount)
	{
		// 画面外に追い出す
		o.svPos = float4(0.0f, 0.0f, -1.0f, 0.0f);
		return o;
	}

	ParticleData p = g_particles[g_particleStartIndex + particleIdx];

	// life <= 0 のスロットは描画しない
	// (CPU経路は常にlife>0のみアップロードするため実質no-op。
	//  GPUプールのように配列が疎になり得る経路で必要)
	if (p.life <= 0.0f)
	{
		o.svPos = float4(0.0f, 0.0f, -1.0f, 0.0f);
		return o;
	}

	//----------------------------------------------------------
	// 寿命比率 [0, 1]（0=生まれたて, 1=消滅直前）
	//----------------------------------------------------------
	const float t = 1.0f - saturate(p.life / p.lifeMax);

	//----------------------------------------------------------
	// サイズ補間(カーブ有効時はLUTサンプル、無効時は2点補間)
	//----------------------------------------------------------
	const float size = (g_sizeUseCurve != 0) ? SampleCurveLUT16(g_sizeCurveLUT, t) : lerp(p.sizeStart, p.sizeEnd, t);

	//----------------------------------------------------------
	// 色補間 + HDR発光強度を乗算
	// アルファのみ、カーブ有効時はLUTサンプルで上書きする
	//----------------------------------------------------------
	o.color = lerp(p.colorStart, p.colorEnd, t);

	if (g_alphaUseCurve != 0)
	{
		o.color.a = SampleCurveLUT16(g_alphaCurveLUT, t);
	}

	o.color.rgb *= p.emissiveIntensity;

	// 発光強度を掛けた後で、受光→フォグの順に適用する(標準シェーダーと同じ順。
	// ブルームへ入る明るさもフォグで弱まる)
	o.color.rgb = ApplyParticleLighting(p.position, o.color.rgb);
	o.color.rgb = ApplyParticleFog(p.position, o.color.rgb);

	o.dissolveT = t;

	//----------------------------------------------------------
	// ビルボード：カメラのビュー行列の Right/Up を使う
	//----------------------------------------------------------
	const float3 camRight = float3(g_mEview[0][0], g_mEview[1][0], g_mEview[2][0]);
	const float3 camUp = float3(g_mEview[0][1], g_mEview[1][1], g_mEview[2][1]);

	float2 rotOffset;

	// renderMode: 0=Billboard, 2=Stretch(ParticleConst.h::RenderMode)
	if (g_renderMode == 2 && g_stretchWorldAligned != 0)
	{
		//------------------------------------------------------
		// ワールド整列ストレッチ：速度方向をカメラ平面へ投影せず、実際の3D方向を
		// そのまま伸長軸として使う(RibbonShaderと同じ考え方)。幅方向のみ
		// 「伸長軸×カメラ視線」で求める(カメラ視線とほぼ平行ならカメラUpで代用)。
		// 通常のStretch(スクリーン投影)と異なりカメラの向きによって伸長軸が
		// 縮んで見えることが無いため、細い線状テクスチャでも視点によらず
		// 実際の進行方向を向き続ける
		//------------------------------------------------------
		const float speed = length(p.velocity);
		float3 velDir3D = (speed > 0.0001f) ? (p.velocity / speed) : float3(0.0f, 1.0f, 0.0f);

		const float3 camForward = float3(g_mEview[0][2], g_mEview[1][2], g_mEview[2][2]);
		float3 widthDir3D = cross(velDir3D, camForward);
		const float widthDirLen = length(widthDir3D);

		if (widthDirLen < 0.0001f)
		{
			widthDir3D = camUp;
		}
		else
		{
			widthDir3D /= widthDirLen;
		}

		const float width = size * g_texAspect;
		const float length_ = size * (1.0f + speed * g_stretchLengthScale);

		const float2 local = kQuadOffset[quadVtx];

		const float3 worldOffset3D = widthDir3D * (local.x * width) + velDir3D * (local.y * length_);

		const float4 viewPosWA = mul(float4(p.position + worldOffset3D, 1.0f), g_mEview);
		o.svPos = mul(viewPosWA, g_mProj);
		o.uv = kQuadUV[quadVtx];

		return o;
	}

	if (g_renderMode == 2)
	{
		//------------------------------------------------------
		// ストレッチビルボード：速度方向をカメラ平面へ投影した向きに
		// 板ポリを伸ばす。速度が0に近い場合はカメラUp方向にフォールバックする
		//------------------------------------------------------
		float2 velDir2D = float2(dot(p.velocity, camRight), dot(p.velocity, camUp));
		const float velLen = length(velDir2D);
		const float speed = length(p.velocity);

		if (velLen < 0.0001f)
		{
			velDir2D = float2(0.0f, 1.0f);
		}
		else
		{
			velDir2D /= velLen;
		}

		const float2 perpDir2D = float2(-velDir2D.y, velDir2D.x);

		const float width = size * g_texAspect;
		const float length_ = size * (1.0f + speed * g_stretchLengthScale);

		const float2 local = kQuadOffset[quadVtx];

		rotOffset = perpDir2D * (local.x * width) + velDir2D * (local.y * length_);
	}
	else
	{
		//------------------------------------------------------
		// 通常のビルボード：アスペクト比補正済みのローカルオフセットに
		// Z回転を適用する
		// Y 方向は size そのまま、X 方向は size × アスペクト比 にする
		// これによりテクスチャの縦横比をワールド空間で保持できる
		//------------------------------------------------------
		float2 offset;
		offset.x = kQuadOffset[quadVtx].x * size * g_texAspect;
		offset.y = kQuadOffset[quadVtx].y * size;

		// 回転角(カーブ有効時はLUTサンプルの角度を直接使い、無効時は
		// CPU側で角速度から積分済みのp.rotationをそのまま使う)
		const float rotationAngle = (g_rotationUseCurve != 0) ? SampleCurveLUT16(g_rotationCurveLUT, t) : p.rotation;
		const float s = sin(rotationAngle);
		const float c = cos(rotationAngle);

		rotOffset.x = offset.x * c - offset.y * s;
		rotOffset.y = offset.x * s + offset.y * c;
	}

	const float3 planeRight = float3(1.0f, 0.0f, 0.0f);
	const float3 planeUp = float3(0.0f, 0.0f, 1.0f);
	const bool useGroundPlane = (g_renderMode == 0) && (g_groundPlaneAligned != 0);
	const float3 basisRight = useGroundPlane ? planeRight : camRight;
	const float3 basisUp = useGroundPlane ? planeUp : camUp;
	const float3 worldPos = p.position
		+ basisRight * rotOffset.x
		+ basisUp * rotOffset.y;

	//----------------------------------------------------------
	// クリップ空間へ変換
	//----------------------------------------------------------
	const float4 viewPos = mul(float4(worldPos, 1.0f), g_mEview);
	o.svPos = mul(viewPos, g_mProj);

	o.uv = kQuadUV[quadVtx];

	return o;
}

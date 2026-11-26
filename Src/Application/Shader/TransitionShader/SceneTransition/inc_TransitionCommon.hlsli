// このファイルはBaseFramework(提供テンプレート)には存在しない、独自追加ファイルです。

//=============================================================
// inc_TransitionCommon.hlsli
// 画面全体を1枚のピクセルシェーダーで描き直す系シェーダーの共通部品。
//
// ここへ置くのは「複数のシェーダーが同じ式で使うもの」だけにする。
// 1本でしか使わない演出固有の計算は、そのシェーダー側へ書く。
//
// 定数バッファは各シェーダーが自分で宣言する。この中の関数は
// 定数バッファを直接読まず、必要な値を引数で受け取る
//=============================================================

#ifndef INC_TRANSITION_COMMON_HLSLI
#define INC_TRANSITION_COMMON_HLSLI

static const float TRANSITION_PANEL_BASE_HEIGHT = 540.0f;	// 描画座標の基準となる高さ(px)
static const float TRANSITION_PANEL_GRID_LINE_WIDTH = 1.2f;	// 格子線の幅(基準高でのpx)
static const float TRANSITION_PANEL_SCAN_WIDTH = 1.4f;	// 走査線の幅(基準高でのpx)
static const float TRANSITION_PANEL_SCAN_BAND_HALF = 0.045f;	// 走査帯の半幅(UV)
static const float3 TRANSITION_PANEL_GRID_LINE_COLOR = float3(0.09f, 0.20f, 0.24f);	// 格子線の既定色
static const float TRANSITION_PANEL_SCAN_BAND_OPACITY = 0.22f;	// 走査帯の既定の不透明度

//--------------------------------------------------------------
// 頂点シェーダーからの入力
// VS / InputLayout は KdPostProcessShader のものを流用しているため、
// この形はトランジション系で共通になる
//--------------------------------------------------------------
struct VSOutput
{
	float4 Pos : SV_POSITION;
	float2 UV  : TEXCOORD0;
};

//--------------------------------------------------------------
// 擬似乱数(frac(sin()) 法)
//--------------------------------------------------------------
float TransitionHash(float v)
{
	return frac(sin(v * 78.233f) * 43758.5453f);
}

float TransitionHash2(float2 v)
{
	return frac(sin(dot(v, float2(12.9898f, 78.233f))) * 43758.5453f);
}

//--------------------------------------------------------------
// 明るさ
// 2つある理由: 演出ごとにどちらの重みで作ったかが見た目に出るため、
// 既存の絵を変えないよう両方を残している。新規は Rec.709 を使う
//--------------------------------------------------------------

// Rec.709。人の感じ方に近く、彩度やトーンを触る用途で使う
float TransitionLuminance709(float3 c)
{
	return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// Rec.601。明暗を面へ分ける用途で使っている
float TransitionLuminance601(float3 c)
{
	return dot(c, float3(0.299f, 0.587f, 0.114f));
}

//--------------------------------------------------------------
// グリッチのUVオフセット
// Y方向を細切れの帯に分割し、一部の帯だけ横方向へずらす。
// 全部の帯を動かすと画面が溶けるため、8%だけに当てる
//
// strength : ずらし幅(UV単位)
// time     : 乱数の種。毎フレーム変わる値を渡す
//--------------------------------------------------------------
float2 CalcGlitchOffset(float2 uv, float time, float strength)
{
	// 帯の高さ(UV単位)
	static const float BAND_HEIGHT = 0.04f;

	const float band = floor(uv.y / BAND_HEIGHT);
	const float r0 = TransitionHash(band + floor(time));
	const float r1 = TransitionHash(band + floor(time) + 1.0f);

	// 8%の帯にだけ適用する
	const float intensity = step(0.92f, r0);

	const float offsetX = (r1 * 2.0f - 1.0f) * strength * intensity;

	return float2(offsetX, 0.0f);
}

//--------------------------------------------------------------
// 色収差
// 中心から離れるほど強く、RとBを逆方向へずらす。
// centerを画面中央のままにすればレンズ収差、画面上の一点へ寄せると
// そこから放射状に色が裂ける歪みになる
//
// strength  : ずらし幅。0で元の色をそのまま返す
// edgePower : 中心からの距離に対する立ち上がり。1未満で中心寄りにも出る
//--------------------------------------------------------------
float4 SampleChromaticAberration(
	Texture2D    tex,
	SamplerState samp,
	float2       uv,
	float2       center,
	float        strength,
	float        edgePower)
{
	float4 result = tex.Sample(samp, uv);

	if (strength > 0.0f)
	{
		const float2 fromCenter = uv - center;

		float edge = saturate(dot(fromCenter, fromCenter) * 4.0f);
		edge = pow(edge, max(edgePower, 0.01f));

		const float2 shift = fromCenter * edge * strength;

		result.r = tex.Sample(samp, uv + shift).r;
		result.b = tex.Sample(samp, uv - shift).b;
	}

	return result;
}

//--------------------------------------------------------------
// 色収差(向きを指定する版)
// 画面のどこでも同じ向き・同じ量でRとBをずらす。
// ワイプの先端やスライスの切り口のように「動いた向き」が決まっている
// 演出では、中心から放射状に出る版より素直に見える
//
// shift : Rをずらす量と向き(UV単位)。Bは逆向きへ同じだけずらす
//--------------------------------------------------------------
float3 SampleChromaticAberrationDirectional(
	Texture2D    tex,
	SamplerState samp,
	float2       uv,
	float2       shift)
{
	float3 result;
	result.r = tex.Sample(samp, saturate(uv + shift)).r;
	result.g = tex.Sample(samp, uv).g;
	result.b = tex.Sample(samp, saturate(uv - shift)).b;

	return result;
}

//--------------------------------------------------------------
// 座標を角度degだけ回転する
//--------------------------------------------------------------
float2 TransitionRotate2D(float2 p, float deg)
{
	const float rad = radians(deg);
	const float s = sin(rad);
	const float c = cos(rad);

	return float2(p.x * c - p.y * s, p.x * s + p.y * c);
}

//--------------------------------------------------------------
// 磁力線グリッド(斜めの格子線)のマスク
// basePx    : 基準解像度へ換算したピクセル座標
// pitch     : 格子の間隔
// angleDeg  : 格子の傾き(度)
// lineWidth : 格子線の太さ
//--------------------------------------------------------------
float CalcGridMask(float2 basePx, float pitch, float angleDeg, float lineWidth)
{
	const float2 p = TransitionRotate2D(basePx, angleDeg);
	const float2 cell = abs(frac(p / pitch) * pitch - pitch * 0.5f);
	const float  lineDist = min(cell.x, cell.y);

	return 1.0f - smoothstep(lineWidth * 0.5f, lineWidth * 0.5f + 0.7f, lineDist);
}

//--------------------------------------------------------------
// 走査線(細い横線)のマスク
// basePxY : 基準解像度へ換算したピクセル座標のY
// pitch   : 線の周期
// width   : 線の太さ
//--------------------------------------------------------------
float CalcScanlineMask(float basePxY, float pitch, float width)
{
	const float cell = frac(basePxY / pitch) * pitch;

	return 1.0f - smoothstep(width - 0.5f, width + 0.5f, cell);
}

//--------------------------------------------------------------
// サイバーパネル
// 「画面を覆いきった面」の見た目をここ1つで決める。
//
// ワイプの覆った側とロード画面の背景が同じ関数を呼ぶため、覆い終わって
// ロード画面へ移る1フレームで絵が変わらない。片方だけ手を入れて
// 食い違うこともなくなる
//--------------------------------------------------------------
struct CyberPanelParams
{
	float3 baseColor;		// 地の色
	float3 gridLineColor;	// 磁力線グリッドの線色
	float3 glowColor;		// 走査線と走査帯の色

	float  gridPitch;		// 格子の間隔
	float  gridAngle;		// 格子の傾き(度)
	float  gridLineWidth;	// 格子線の太さ
	float  gridOpacity;		// 格子の濃さ

	float  scanPitch;		// 走査線の周期
	float  scanWidth;		// 走査線の太さ
	float  scanOpacity;		// 走査線の濃さ

	float  scanBandY;		// 流れる走査帯の中心(UVのY)。画面外なら出ない
	float  scanBandHalf;	// 走査帯の半幅(UV)
	float  scanBandOpacity;	// 走査帯の濃さ

	float  vignette;		// 画面端の落ち込み(0で無効)
};

// basePx : 基準解像度へ換算したピクセル座標
// uv     : 走査帯の位置と画面端の落ち込みを測るためのUV
float3 CalcCyberPanel(CyberPanelParams p, float2 basePx, float2 uv)
{
	// 斜めの格子。面に方向を与える
	const float gridMask = CalcGridMask(basePx, p.gridPitch, p.gridAngle, p.gridLineWidth);

	// 細い横線。面に走査されている質感を与える
	const float scanMask = CalcScanlineMask(basePx.y, p.scanPitch, p.scanWidth);

	// ゆっくり縦に流れる明るい帯。静止画に見せないための唯一の動き
	const float bandMask = 1.0f - smoothstep(0.0f, max(p.scanBandHalf, 1e-4f), abs(uv.y - p.scanBandY));

	float3 col = lerp(p.baseColor, p.gridLineColor, gridMask * p.gridOpacity);

	col += p.glowColor * scanMask * p.scanOpacity;
	col += p.glowColor * bandMask * p.scanBandOpacity;

	// 画面端をわずかに落とし、中央へ目を集める
	const float2 centered = uv - 0.5f;
	col *= 1.0f - saturate(dot(centered, centered) * 2.0f) * p.vignette;

	return col;
}

//--------------------------------------------------------------
// 明るさの段差から輪郭を拾う
// 上下左右の1画素と比べ、差が閾値を超えた分を0〜1へ正規化する
//
// texelSize : 1画素ぶんのUV
// threshold : ここを超えた段差だけ輪郭とみなす
//--------------------------------------------------------------
float CalcLuminanceEdge(
	Texture2D    tex,
	SamplerState samp,
	float2       uv,
	float2       texelSize,
	float        threshold)
{
	const float l = TransitionLuminance601(tex.Sample(samp, uv - float2(texelSize.x, 0.0f)).rgb);
	const float r = TransitionLuminance601(tex.Sample(samp, uv + float2(texelSize.x, 0.0f)).rgb);
	const float u = TransitionLuminance601(tex.Sample(samp, uv - float2(0.0f, texelSize.y)).rgb);
	const float d = TransitionLuminance601(tex.Sample(samp, uv + float2(0.0f, texelSize.y)).rgb);

	return saturate(
		(abs(r - l) + abs(d - u) - threshold) / max(1.0f - threshold, 1e-4f));
}

#endif

# MAGNETIA REPULSE ソースコード抜粋

磁力で戦う TPS 3D アクション「MAGNETIA REPULSE」から、[ポートフォリオ](https://nonaka-06.github.io/works/magnetia.html)で紹介している仕組みのコードだけを抜き出したものです。
プロジェクト全体ではないため、このリポジトリだけではビルドできません。

## 収録しているコード

| ポートフォリオの章 | ファイル | 内容 |
|---|---|---|
| エディターの工夫と改善 | `Src/Application/Editor/ParticleEditor/ParticleEffectBrowser.*` | 演出一覧の検索・タグでの絞り込み |
| 描画処理の負荷軽減(リングバッファ・頂点プル) | `Src/Application/Shader/ParticleShader/Particle/ParticleShader.*`、`Particle_VS.hlsl`、`../ParticleGpuLayout.hlsli` | 粒子データをリングバッファへ追記し、頂点シェーダーで読み出して描く |
| メッシュ粒子のインスタンシング | `Src/Application/Render/KdDrawPassRenderer.*` | 同じモデルをまとめて1回の描画命令で描く |
| 発展：GPUパーティクル | `Src/Application/Effect/Particle/Gpu/ParticleGpuPool.*`、`ParticleEmit_CS.hlsl`、`ParticleUpdate_CS.hlsl` | 空き番号の管理と、コンピュートシェーダーでの放出・更新 |
| ポストプロセス | `Src/Framework/Shader/PostProcessShader/KdPostProcessShader_PS_HighlightCompress.hlsl` | ブルーム前の白飛びの抑制 |
| ポストプロセス | `Src/Application/Shader/TransitionShader/PostProcess/CinematicGrading*`、`../SceneTransition/inc_TransitionCommon.hlsli` | 色収差・グレーディング |

フォルダ構成は元のプロジェクトのままです。素材・外部ライブラリ・ほかのソースは含みません。

Copyright (C) 2026 Shunsuke Nonaka. All rights reserved.

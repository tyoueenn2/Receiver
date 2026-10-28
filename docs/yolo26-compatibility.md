# YOLO26 raw detection compatibility

調査対象: Receiver `0cf206cc7b8741d72060e8fa30b8500111de6a08`（取得時の main）。
公式実装の参照: Ultralytics `bb3e1550dbb819bd9846d4c99f368aebe8792a0e`、8.4.191。

## 結論と変更

- **今回の公式 YOLO26s checkpoint は、Ultralytics 8.4.191 では変更前の Receiver ワーカーでも読み込める。** 320/640 の実推論で、`end2end=False`、80 クラス、2100/8400 候補を確認した。YOLO26 の `.pt` が一律に非互換という結論ではない。
- 既存 exporter の `nms=False` は、現在の Ultralytics では一対一ヘッドを選ぶ。Receiver は `xywh + クラス別スコア` を要求するので、**`nms=None` に修正**した。`nms=True` による NMS 内蔵も使わない。
- `select_raw_detection()` は有効な end-to-end モードを検出した場合のみ、残存する `Detect.cv2/cv3` 一対多ヘッドを選択する。network/head 両方を設定し、選択不能、融合で一対多ヘッドを削除したモデル、`xyxy` 出力は拒否する。YOLO11/Omni の通常経路と実テンソルの形状検証は維持する。
- ONNX の `end2end=True` は動的形状でも拒否し、`False` は C++ 内で bool に正規化する。engine metadata は `nms:null` を許容し、`end2end:true` は拒否する。後者は最新形式のメタデータとの互換性に必要。
- `raw_yolo11` は既存の出力レイアウト識別子として維持する。YOLO26 用の別デコーダーや新しい manifest 名は不要。C++ の座標復元とクラス別 NMS は変更しない。

## 実重みで確認した範囲

CPU 環境: Python 3.12、Ultralytics 8.4.191、PyTorch 2.14.1+cu130（実行デバイスは CPU）、ONNX 1.23.2、ONNX Runtime 1.31.1。

| checkpoint | 入力 | 出力 | クラス | PyTorch/C++ ワーカー・座標・NMS |
|---|---|---|---|---|
| YOLO26s | `[1,3,320,320]` | `[1,84,2100]` | 80 | 成功 |
| YOLO26s | `[1,3,640,640]` | `[1,84,8400]` | 80 | 成功 |
| YOLO11s | `[1,3,320,320]` | `[1,84,2100]` | 80 | 成功 |
| YOLO11s | `[1,3,640,640]` | `[1,84,8400]` | 80 | 成功 |

実画像は Ultralytics の bus.jpg を 319×241 に変換し、奇数・非正方形の letterbox を検証した。
前処理を独立した bilinear 実装と比較し、C++→TCP→Python の実プロセス出力を直接 PyTorch と比較した。
実画像の検出結果は torchvision のクラス別 NMS を使う参照結果と比較した。
YOLO26 の実モデルを一対一モードに切り替えてから一対多に戻すテスト、および一対一融合済みモデルの拒否も成功した。

ONNX の実行検証も全行列で成功した（終了コード 0）:

| checkpoint | エクスポートサイズ | 固定 ONNX | 動的 ONNX |
|---|---|---|---|
| YOLO26s | 320 | 320 推論成功 | 320/640 推論成功 |
| YOLO26s | 640 | 640 推論成功 | 640/320 推論成功 |
| YOLO11s | 320 | 320 推論成功 | 320/640 推論成功 |
| YOLO11s | 640 | 640 推論成功 | 640/320 推論成功 |

合計 8 エクスポート、12 組の ONNX Runtime 推論を確認。
エクスポートサイズでの出力は PyTorch と比較（絶対許容誤差 .003、相対 .0002）。
全 ONNX 推論で形状・有限値、C++ の座標復元と独立した NMS 参照結果を確認した。
各 manifest のクラス名・サイズ・SHA-256 と C++ inspector も検証した。
実行ログ: `build/ci/yolo-compatibility-complete.log`。

チェックポイント SHA-256:

```text
yolo26s.pt 646f8bc3fe0a656803d95c294f7852321748cb29d13466a1af8862e2db384a1b
yolo11s.pt 85a76fe86dd8afe384648546b56a7a78580c7cb7b404fc595f97969322d502d5
```

## 合成データ・メタデータ・既存テスト

- 320/640、2 クラスの合成 raw tensor で、元画像の既知の箱への座標復元、同クラス重複の抑制、別クラスの重なりの保持、低信頼度除去を C++ デコーダーで確認。
- `[1,300,6]` とクラス数不一致の worker 出力を拒否。
- 公式 ONNX スキーマで生成した metadata テスト 25 件成功。実学習済みモデルによる精度検証ではない。
- Linux Release の既存 CTest は **28 件中 24 件成功、4 件失敗**。変更前の独立 worktree でも同じ 4 件が失敗した。
- 失敗: `receiver_process_identity`, `receiver_color_pipeline`, `receiver_class_preference_pipeline`, `receiver_model_faults`。この実行環境では `os.getpid()` に対応する `/proc/<pid>/stat` がなく、fixture の process identity 取得が失敗する。これを隠すためにテストや製品コードは変更していない。
- GitHub Actions には独立した `yolo-compatibility` job を追加した。既存 Omni job の Ultralytics 8.3.199 環境は変更していない。上記の結果はローカル実行によるもので、GitHub Actions の結果とは区別する。
- CUDA/TensorRT 実機、Windows GUI、YOLO-Omni 実重み、任意の学習済みカスタム checkpoint、精度・速度比較は未検証。

## 利用手順

YOLO26 用には Omni の依存環境と分離した Python 環境を用意する。例:

```sh
python -m venv .venv-yolo26
# OS に応じてこの環境を有効化し、CPU/CUDA に合う PyTorch をインストールする。
python -m pip install ultralytics==8.4.191 onnx==1.23.2
```

`.pt` を直接使う場合は、取得済みの checkpoint を指定し、`omni_python` をその環境の Python の絶対パス、`omni_source` を空文字列にする。`input_size` は 320 または 640、厳密なサイズ比較には `auto_size:false` を指定する。CPU 検証では `omni_device:"cpu"` を使う。既存の Omni fork を `omni_source` に残すと YOLO26 のクラスを読み込めない可能性がある。

修正後の exporter:

```sh
python tools/export_model.py --weights /path/to/yolo26s.pt --size 320 --fixed
python tools/export_model.py --weights /path/to/yolo26s.pt --size 640 --fixed
# --fixed を省くと動的入力。上記2回は同じ .onnx/.json を上書きするので、
# 両サイズを保存する場合は重みを別ディレクトリにコピーしてから実行する。
```

`.onnx` と隣接する `.onnx.json` をセットで使用する。TensorRT engine は対象 GPU で構築し、実機検証を行う。汎用 Ultralytics の CLI を使う場合も、現行版では `nms=False` を指定せず、raw の出力形状とメタデータを確認する。古い版では引数の意味が異なるため、今回の検証結果を未検証バージョンに一般化しない。

## テストの再実行

```sh
cmake -S . -B build/ci -DCMAKE_BUILD_TYPE=Release -DRECEIVER_GUI=OFF -DRECEIVER_TENSORRT=OFF -DRECEIVER_INTEGRATION_TESTS=ON
cmake --build build/ci --parallel 4
ctest --test-dir build/ci --output-on-failure
python tests/model_metadata_oracle_test.py build/ci/receiver_inspect_model
python tests/yolo_raw_compatibility_test.py --native-only \
  --weights /path/to/yolo26s.pt --weights /path/to/yolo11s.pt \
  --image /path/to/bus.jpg --decoder build/ci/receiver_decode \
  --inspector build/ci/receiver_inspect_model --verify-exe build/ci/receiver_verify
```

`--native-only` を省くと、ONNX 固定/動的・両サイズの全行列を実行する。テスト自体は重みをダウンロードしない。

当初、ONNX Runtime の初期化時の Microsoft テレメトリー送信により自動承認レビューが実行継続を拒否した。
公式資料を確認した結果、API の `disable_telemetry_events()` だけでは初期化イベントを防げないことが分かった。
`ORT_DISABLE_TELEMETRY=1` を **ONNX Runtime 初期化前** に設定して全プロセスに継承することで、公式仕様上 uploader・イベント・永続識別子の作成を無効化できる。
回帰テストにこの設定を組み込み、その状態で全行列の実行が完了した。外部送信を許可して強行した結果ではない。
単独で exporter を使う場合も、起動前に同じ環境変数を設定できる（bash: `export ORT_DISABLE_TELEMETRY=1`、PowerShell: `$env:ORT_DISABLE_TELEMETRY="1"`）。

参考:

- https://docs.ultralytics.com/guides/end2end-detection
- https://github.com/ultralytics/ultralytics/blob/bb3e1550dbb819bd9846d4c99f368aebe8792a0e/ultralytics/engine/exporter.py
- https://github.com/ultralytics/ultralytics/blob/bb3e1550dbb819bd9846d4c99f368aebe8792a0e/ultralytics/nn/modules/head.py

- https://github.com/microsoft/onnxruntime/blob/main/docs/Privacy.md

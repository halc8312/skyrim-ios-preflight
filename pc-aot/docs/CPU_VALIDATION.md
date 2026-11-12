# 変換器の命令比較

Skydream `ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a` のAF（補助キャリーフラグ）欠落を実際のx86命令との比較で再現し、独立パッチで修正しました。購入済みゲームのコードはこの検査に使いません。**WSL x86_64での結果であり、ARM64・iPhone・ゲーム全体の正しさは未検証です。**

## 比較方法と結果

`scripts/check_lifter_flags.py` は自作の小さなPEと命令列を生成し、固定commitの変換器が出力したCをGCCでコンパイルします。`tests/native_x86_flags.c` は同じ演算を実CPUの8/16/32-bit命令で実行し、結果と定義済みのCF/PF/AF/ZF/SF/OFを比較します。元のhost flagsは毎回復元し、TF/DF/ACは有効にしません。JIT、Unicorn、ゲームexeの実行は使いません。

| 検査 | ケース数（各最適化） | O0不一致 | O2不一致 |
|---|---:|---:|---:|
| 最初のADD/SUB/ADC/SBB/INC/DEC比較 | 8,176 | 3,436 | 3,436 |
| 拡張検査・上流のまま | 40,632 | 13,016 | 13,016 |
| 拡張検査・AFパッチ適用 | 40,632 | 0 | 0 |

最初の再現例は `ADD AL,CL`、`01 + 0F → 10`。演算結果は一致しましたが、実CPUのEFLAGS `0x212` に対し、生成Cは `0x202` となり、AFのbit4が欠落しました。拡張検査ではLAHFがAFをAHへ写さないため、96ケースでEAXにも差が出ました。これは13,016不一致ケースの一部です。

拡張範囲はCMP/NEGを含む整数演算、INC/DECのCF保持、全64通りの算術フラグ組合せ、SAHFの全256 AH値、CLC/STC/CMC/MOVのAF保持、有限値でのCOMISS/UCOMISS/COMISD/UCOMISDとFCOMI/FUCOMI/FCOMIP/FUCOMIPによるAFクリアです。ECXとスタック位置の保持も確認します。

## 修正と再現

`patches/skydream/0001-auxiliary-carry.patch` は `runtime/x86.h` と `tools/lift.py` だけを変更します。AFの保存・演算・転送を追加し、AFを0にする仕様の比較命令も更新しました。未定義のAF値を正解として固定しません。

パッチSHA-256は `110850aabfe70b26220557f528e54fe75a13ef5b38cb4ace7fbbd4ac9e5035ad`。`source-lock.json` に基底commitと順序を固定し、プリフライトの生成contractにも含めます。`.gitattributes` はパッチをLFに固定します。

```sh
cd /mnt/f/SkyrimIOS
private/tooling/skydream-py312-20261112/bin/python3 -E -s -B scripts/check_lifter_flags.py
```

各実行は新しい `local/tests/lifter-flags-*/` を作り、改変前／修正後を別のcanonical `git archive` から用意します。適用先に独立Git管理を作り、ハッシュ・適用検査・変更後ファイルを記録します。vendorや過去の検査結果は変更しません。exit0は修正版成功と上流不具合の再現、exit1は比較条件不成立、exit2は検査準備／ビルド失敗です。

証拠は次の場所にあります。

- 初回再現: `local/tests/lifter-flags-20261112T075445.160484Z-58/report.json`。
- 拡張比較: `local/tests/lifter-flags-20261112T080014.624903Z-58/report.json` と各variantの原出力・生成C・不一致一覧。
- 修正後 `runtime/x86.h`: SHA-256 `13878e1c48d65e3b7b0162570f8df2c4576560060c3eda1f52cdd66869004302`。
- 修正後 `tools/lift.py`: SHA-256 `c0f31459bb311cc4c12de668f8c4ffe44a973dc59c5ecbc9b2d3f38b1610fd3b`。

初回の厳格コンパイルは上流header内の既存警告で失敗し、そのログも保持しました。検査コードと生成Cは `-Wall -Wextra -Werror` を維持し、上流headerだけを `-isystem` としました。比較成功後、archive時の `core.autocrlf=false`、依存の正確な版チェック、適用先設定、変更後ハッシュ確認を明示しました。演算パッチと比較ケースは成功時と同じです。

## 未解決の範囲

元runtimeのflagsは関数内のローカル状態で、guest callをまたぐ保存／復元はこの修正に含みません。未定義flags、NaN・非マスク例外、x87拡張精度、MXCSR、メモリーoperandやatomic、ARM64メモリー順序も別の検査が必要です。有限値の浮動小数点比較はAFクリアの確認であり、浮動小数点全般の適合性検査ではありません。ゲーム入力の互換profileは未承認のままです。

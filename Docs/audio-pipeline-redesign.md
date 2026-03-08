# Audio Pipeline Redesign — SoundWaveProcedural 方式

> Status: **Ready** — 設計完了、実装待ち
> Date: 2026-03-08
> Related: [Link Audio SDK 4.0.0-beta2](https://github.com/Ableton/link)

## 背景

現行の AudioBus + PatchInput 方式は以下の問題を抱えている:

- ユーザーが **AudioBus アセット** と **SoundSourceBus アセット** を手動で作成する必要がある
- SoundSourceBus を再生する手段（AmbientSound 配置や PlaySound2D 呼び出し）も別途必要
- 設定 UI と実際の再生までの手順が多く、UX が悪い
- AudioBus の PatchInput/PatchMixer は本来オーディオエンジン内部のルーティング用で、外部オーディオ注入には不向き

AudioCaptureComponent の調査から、**USoundWaveProcedural** を使った直接バッファ注入が UE の正規パターンであることが判明した。

---

## 新アーキテクチャ概要

### Receive (Link Audio → UE)

```
Link Audio Source callback (ネットワークスレッド, int16 interleaved)
    |
    v
[リサンプリング (SrcRate != DeviceRate の場合、線形補間)]
    |
    v
USoundWaveProcedural::QueueAudio()   <-- スレッドセーフ (内部 TQueue)
    |
    v
Audio Mixer: GeneratePCMData() でデキュー (オーディオスレッド)
    |
    v
FActiveSound (FAudioDevice::AddNewActiveSound() で登録、World/Actor/Component 不要)
    |
    v
USoundSubmix (設定で指定。空 = Master Submix)
    |
    v
スピーカー出力
```

**ポイント**: UAudioComponent は使わない。FActiveSound を直接構築して AudioDevice に
登録する。World=nullptr、bAllowSpatialization=false で非空間オーディオとして再生。

### Send (UE → Link Audio) — 現行維持

```
USoundSubmix (設定で指定。空 = Master Submix)
    |
    v
ISubmixBufferListener::OnNewSubmixBuffer() (オーディオスレッド, float interleaved)
    |
    v
float → int16 変換
    |
    v
Link Audio Sink::commit()
```

Send 側は現行の `FLink4UESendBridge` (ISubmixBufferListener) をそのまま使う。

---

## Settings 構造体

### Send

```cpp
USTRUCT(BlueprintType)
struct FLink4UEAudioSend
{
    GENERATED_BODY()

    /** キャプチャ元 Submix。空 = Master Submix。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Link4UE")
    TSoftObjectPtr<USoundSubmix> Submix;

    /** Link ネットワーク上のチャンネル名。空 = Submix アセット名を使用。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Link4UE")
    FString ChannelNamePrefix;
};
```

**変更なし** — 現行と同一。

### Receive

```cpp
USTRUCT(BlueprintType)
struct FLink4UEAudioReceive
{
    GENERATED_BODY()

    /** 受信する Link Audio チャンネル名。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Link4UE")
    FString ChannelName;

    /** 出力先 Submix。空 = Master Submix (デフォルト動作)。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Link4UE")
    TSoftObjectPtr<USoundSubmix> Submix;
};
```

**AudioBus / SoundSourceBus は完全に不要になる。**

---

## ReceiveBridge 新設計

### 設計原則: 1 チャンネル = 1 Bridge

SDK の `MainProcessor::receiveAudioBuffer()` は `std::find_if()` で最初にマッチした
Source にだけバッファを渡す。同一 ChannelId に複数の Source を作っても 2 つ目以降は
音声を受信できない。

そのため **1 つの Link Audio チャンネルに対して 1 つの ReceiveBridge (= 1 LinkAudioSource)**
を保証する。同じチャンネルを複数の Submix に出力したい場合は、1 Bridge 内で複数の
出力先 (ProceduralSound + FActiveSound のペア) を持ち、コールバック内で全出力先に
`QueueAudio()` する。

`RebuildAudioReceives` で設定エントリを ChannelName でグルーピングし、
同一チャンネルのエントリは 1 つの Bridge にまとめる。

```cpp
class FLink4UEReceiveBridge
{
public:
    // 出力先 (1 Bridge に複数持てる)
    struct FOutput
    {
        TStrongObjectPtr<USoundWaveProcedural> ProceduralSound;
        uint64 AudioComponentID = 0;  // FActiveSound 停止用ハンドル (#9)
    };

    FLink4UEReceiveBridge(ableton::LinkAudio& InLink,
                          const ableton::ChannelId& InChannelId,
                          int32 InNumChannels,
                          int32 InSampleRate,
                          FAudioDevice* InAudioDevice);
    ~FLink4UEReceiveBridge();

    /** 出力先を追加。同じチャンネルを複数 Submix に送る場合に使用。 */
    void AddOutput(USoundSubmix* TargetSubmix, FAudioDevice* AudioDevice);

private:
    void OnSourceBuffer(ableton::LinkAudioSource::BufferHandle Handle);

    ableton::LinkAudioSource Source;
    TArray<FOutput> Outputs;

    int32 DeviceSampleRate;
    int32 NumChannels;
    TArray<int16> ResampleBuffer;  // リサンプリング用ワークバッファ

    FString ChannelName;
};
```

### AddOutput (出力先の追加)

```cpp
void FLink4UEReceiveBridge::AddOutput(USoundSubmix* TargetSubmix,
                                       FAudioDevice* AudioDevice)
{
    FOutput& Out = Outputs.AddDefaulted_GetRef();

    USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>();
    Wave->SetSampleRate(DeviceSampleRate);
    Wave->NumChannels = NumChannels;
    Wave->Duration = INDEFINITELY_LOOPING_DURATION;
    Wave->SoundGroup = SOUNDGROUP_Default;
    Wave->bLooping = true;
    Out.ProceduralSound.Reset(Wave);

    TSharedRef<FActiveSound> NewActiveSound = MakeShared<FActiveSound>();
    NewActiveSound->SetSound(Wave);
    NewActiveSound->SetWorld(nullptr);
    NewActiveSound->bAllowSpatialization = false;
    NewActiveSound->bIsUISound = true;
    NewActiveSound->bLocationDefined = false;

    // 一意な AudioComponentID を設定（停止用ハンドル、懸念点 #9 参照）
    static uint64 NextBridgeSoundID = 0xFF00000000000000ULL;
    Out.AudioComponentID = ++NextBridgeSoundID;
    NewActiveSound->AudioComponentID = Out.AudioComponentID;

    if (TargetSubmix)
    {
        FSoundSubmixSendInfo SubmixSend;
        SubmixSend.SoundSubmix = TargetSubmix;
        SubmixSend.SendLevel = 1.0f;
        NewActiveSound->SetSubmixSend(SubmixSend);
    }

    AudioDevice->AddNewActiveSound(MoveTemp(NewActiveSound));
}
```

### OnSourceBuffer コールバック

```cpp
void FLink4UEReceiveBridge::OnSourceBuffer(BufferHandle Handle)
{
    const int32 SrcRate = Handle.info.sampleRate;
    const int32 SrcFrames = Handle.info.numFrames;
    const int32 SrcChannels = Handle.info.numChannels;

    const uint8* AudioData;
    int32 AudioBytes;

    if (SrcRate == DeviceSampleRate)
    {
        AudioData = reinterpret_cast<const uint8*>(Handle.samples);
        AudioBytes = SrcFrames * SrcChannels * sizeof(int16_t);
    }
    else
    {
        // リサンプリング (線形補間)
        const double Ratio = (double)SrcRate / (double)DeviceSampleRate;
        const int32 DstFrames = (int32)(SrcFrames / Ratio);
        ResampleBuffer.SetNumUninitialized(DstFrames * SrcChannels);

        for (int32 DstFrame = 0; DstFrame < DstFrames; ++DstFrame)
        {
            const double SrcPos = DstFrame * Ratio;
            const int32 Idx0 = (int32)SrcPos;
            const int32 Idx1 = FMath::Min(Idx0 + 1, SrcFrames - 1);
            const double Frac = SrcPos - Idx0;

            for (int32 Ch = 0; Ch < SrcChannels; ++Ch)
            {
                const int16 S0 = Handle.samples[Idx0 * SrcChannels + Ch];
                const int16 S1 = Handle.samples[Idx1 * SrcChannels + Ch];
                ResampleBuffer[DstFrame * SrcChannels + Ch] =
                    (int16)(S0 + (int16)((S1 - S0) * Frac));
            }
        }

        AudioData = (const uint8*)ResampleBuffer.GetData();
        AudioBytes = DstFrames * SrcChannels * sizeof(int16);
    }

    // 全出力先にコピー
    for (FOutput& Out : Outputs)
    {
        Out.ProceduralSound->QueueAudio(AudioData, AudioBytes);
    }
}
```

---

## 懸念事項と検討課題

### 1. サンプルレートの不一致 [解決済み]

| 送信元 | サンプルレート |
|--------|---------------|
| Ableton Live | 96000 Hz (ログで確認済み) |
| UE AudioDevice | 通常 48000 Hz |

**調査結果**: `USoundWaveProcedural::SetSampleRate()` は**メタデータ設定のみ**。UE のオーディオミキサーは source の SampleRate と device の SampleRate を比較するロジックを持たない。96kHz データを QueueAudio して 48kHz デバイスで再生すると**半速再生になる**。

**根拠** (UE 5.7 ソース):
- `FMixerSourceBufferInitArgs::SampleRate` には常に `AudioDevice->GetSampleRate()` が渡される（AudioMixerSource.cpp:909）
- リサンプリングは `FRuntimeResampler` 経由のみで、トリガーは `FinalPitch != 1.0f` のときだけ（AudioMixerSourceManager.cpp:2675）
- source の SampleRate と device の SampleRate の比率を自動適用するコードは存在しない

**決定: OnSourceBuffer 内で手動リサンプリング**

理由:
- Pitch 補正 (`SetPitchMultiplier(2.0)`) は簡単だが、アッテネーションやドップラー等の音響処理に副作用がある
- 96k→48k は 2:1 の整数比なので、フレーム間引き + 線形補間で十分な品質
- SDK コールバック内で変換すれば QueueAudio に渡すデータは常にデバイスレート

**実装**: OnSourceBuffer コールバック内で線形補間リサンプリング（上記 ReceiveBridge 設計セクション参照）。

**注意**: Ableton のサンプルレートはユーザー設定次第で変わる (44100, 48000, 96000 等)。
整数比でない場合 (例: 44100→48000) も線形補間で対応可能。

### 2. チャンネル数の動的変更

Link Audio チャンネルの ch 数はセッション中に変わる可能性がある（実用上は stereo 固定だが SDK 上は可変）。

**対応案**:
- `USoundWaveProcedural::SetNumChannels()` は初期化時に設定。途中変更不可
- チャンネル数が変わったら ReceiveBridge を破棄→再作成
- 頻繁に発生しないため、現実的にはこれで十分

### 3. UObject ライフタイム管理 [解決済み]

`USoundWaveProcedural` は UObject なので GC 対象。
`FLink4UEReceiveBridge` は plain C++ class (UObject ではない) なので UPROPERTY が使えない。

**決定: `TStrongObjectPtr<T>` を使用**

`TStrongObjectPtr` は UE 標準の GC 防止スマートポインタ。内部で ref-count を管理し、
デストラクタで自動解放する。スレッドセーフ、コンテナ格納可能、RAII 準拠。

エンジン内の使用例: `UAudioBusSubsystem` が `TArray<TStrongObjectPtr<UAudioBus>>` で
AudioBus を保持している — まったく同じパターン。

```cpp
TStrongObjectPtr<USoundWaveProcedural> ProceduralSound;
// 作成
ProceduralSound.Reset(NewObject<USoundWaveProcedural>());
// デストラクタで自動解放 — RemoveFromRoot() 等は不要
```

不採用の選択肢:
- `AddToRoot()` — ゲームスレッド限定、手動 RemoveFromRoot() が必要で漏れリスクあり
- `NewObject(Outer)` + UPROPERTY — plain C++ class では UPROPERTY 不可
- `FGCObjectScopeGuard` — スコープ限定用、長寿命オブジェクトには不向き

**注意**: `FActiveSound` は UObject ではない (plain struct)。`FAudioDevice::AddNewActiveSound()` で
デバイスに所有権が移る。Bridge はポインタを保持するが、ライフタイムは AudioDevice が管理。

### 4. World コンテキストと再生方式 [解決済み]

**決定: `FActiveSound` + `FAudioDevice::AddNewActiveSound()` を使用**

UAudioComponent も SpawnSound2D も不要。FActiveSound を直接構築して AudioDevice に登録する。
これが最もシンプルで、World/Actor/Component のいずれも不要。

**根拠**:
- `FActiveSound::SetWorld(nullptr)` は安全 — World チェックは `&&` ガードで nullptr なら通過
- `bAllowSpatialization = false` で非空間オーディオとして処理（距離減衰なし）
- `bIsUISound = true` で World ライフタイムに依存しない
- Submix ルーティングは `FActiveSound::SetSubmixSend()` で設定可能

USoundWaveProcedural にデータを push → ミキサーが GeneratePCMData で pull → Submix に流れる。
この3ステップだけが本質。中間にコンポーネントやアクターは不要。

### 4b. 不採用の代替案

| 方式 | 不採用理由 |
|------|-----------|
| `UAudioComponent` + `SpawnSound2D` | World コンテキスト必要、Actor 生成、不要な抽象レイヤー |
| `FAudioDevice::CreateComponent()` | World 不要だが UAudioComponent を介す点は変わらない |
| `SoundSourceBus` + `AudioBus` | バッファ注入 API なし (QueueAudio 不可)。AudioBus + FPatchInput を中間に必要とし、ユーザーがアセットを手動作成する必要がある |

**SoundSourceBus の調査結果**:
`USoundSourceBus` は `USoundWave` から派生 (`USoundWaveProcedural` ではない)。
`QueueAudio()` を持たず、`AudioBus` 経由でしかデータを受け取れない。
データ注入には `UAudioBusSubsystem::AddPatchInputForAudioBus()` → `FPatchInput` → `FPatchMixer` (AudioBus 内部) という
3段階の中間オブジェクトが必要。これは `SignalProcessing` モジュールの `MultithreadedPatching.h` で定義された
リングバッファベースのスレッドセーフパッチシステム。

対して USoundWaveProcedural は `QueueAudio()` (内部 TQueue) で直接バッファ注入が可能。

### 5. Master Send (自動 Sink) との関係 [問題なし]

Link Audio 有効時に Master Submix の Sink を自動作成する現行動作をそのまま維持。
SDK のピアディスカバリプロトコル上、最低 1 つの Sink が必要（詳細は [link-audio-sink-requirement.md](link-audio-sink-requirement.md) 参照）。

新パイプラインへの影響はない:
- Master Send は無条件作成（SDK 要件）
- Receive の出力先が Master Submix でもフィードバックは起きない
  （Master Send は独自のチャンネル名で送出するため、リモートピアが明示的に購読しない限りループしない）
- ユーザーが AudioSends に Master Submix を追加した場合も、別チャンネル名で二重に送出されるだけで破綻しない

### 6. エディタ設定カスタマイズの更新 [最小変更で対応可能]

現行の `FLink4UEAudioReceiveCustomization` は ChannelName をカスタムドロップダウンに差し替えているが、
AudioBus プロパティはデフォルトピッカー（`AddChildContent`）で描画している。

`TSoftObjectPtr<UAudioBus>` → `TSoftObjectPtr<USoundSubmix>` に型を変えるだけで
エディタが自動的に Submix ピッカーを表示する。カスタマイゼーションのロジック変更は不要。

**変更ファイル一覧**:

| ファイル | 変更内容 |
|---------|---------|
| `Link4UESettings.h` | `TSoftObjectPtr<UAudioBus> AudioBus` → `TSoftObjectPtr<USoundSubmix> Submix`、include 変更 |
| `Link4UESettingsCustomization.cpp` | プロパティハンドル名 `"AudioBus"` → `"Submix"` |
| `Link4UESubsystem.cpp` | `RebuildAudioReceives` 内の AudioBus 参照を Submix に |
| `DefaultGame.ini` | 既存設定がある場合は手動移行（プロパティ名変更） |

ChannelName ドロップダウン（`GetChannels()` → `SComboBox`）は変更なし。

### 7. 複数 Receive が同一チャンネルを参照するケース [設計に組み込み済み]

上記 ReceiveBridge の「1 チャンネル = 1 Bridge、複数出力先」設計で対応済み。

### 8. Hot Reload (エディタでの設定変更)

Project Settings で Audio Receives を編集すると `OnSettingsChanged` → `RebuildAudioReceives` が走る。
このとき古い Bridge を壊して新しい Bridge を作り直す必要がある。

**破棄の順序が重要**:

```
1. LinkAudioSource を破棄 → SDK コールバック (OnSourceBuffer) が停止
2. FActiveSound を停止 → ミキサーが GeneratePCMData() を呼ばなくなる
3. TStrongObjectPtr を解放 → USoundWaveProcedural が GC 対象になる
```

この順番を守らないと、ステップ 3 で ProceduralSound が破棄された後に
ステップ 2 の GeneratePCMData() がまだ走っていてクラッシュする可能性がある。

**現行コードで既に対応済みの点**:
- `bIsRebuilding` ガードで、リビルド中に `bChannelsDirty` が立っても再帰しない
- `TearDownReceives()` で全 Bridge を `TUniquePtr::Reset()` → デストラクタで上記順序を実行

### 9. FActiveSound の停止方法 [解決済み]

**調査結果**: UAudioComponent なしでも `AudioComponentID` を手動設定すれば停止可能。

UE 5.7 の `AddNewActiveSound` は `TSharedRef<FActiveSound>` を受け取る新 API がある
（const ref 版は deprecated）。戻り値は void だが、事前に ID を設定しておけばよい。

**停止 API**:
- `FAudioDevice::StopActiveSound(uint64 AudioComponentID)` — Game/Audio どちらのスレッドからでも呼べる（内部で自動ディスパッチ）
- `AudioComponentIDToActiveSoundMap` は `AudioComponentID > 0` の場合にエントリが作られる
- UAudioComponent 由来でなくても任意の uint64 を設定可能

**決定: AudioComponentID 方式**

```cpp
struct FOutput
{
    TStrongObjectPtr<USoundWaveProcedural> ProceduralSound;
    uint64 AudioComponentID = 0;  // 停止用ハンドル
};

void AddOutput(USoundSubmix* TargetSubmix, FAudioDevice* AudioDevice)
{
    // ... ProceduralSound 作成 ...

    TSharedRef<FActiveSound> NewActiveSound = MakeShared<FActiveSound>();
    NewActiveSound->SetSound(Wave);
    NewActiveSound->SetWorld(nullptr);
    NewActiveSound->bAllowSpatialization = false;
    NewActiveSound->bIsUISound = true;

    // 一意な ID を生成して設定
    static uint64 NextBridgeSoundID = 0xFF00000000000000ULL;
    uint64 ID = ++NextBridgeSoundID;
    NewActiveSound->AudioComponentID = ID;
    Out.AudioComponentID = ID;

    AudioDevice->AddNewActiveSound(MoveTemp(NewActiveSound));
}

// 破棄時
void RemoveOutput(FOutput& Out, FAudioDevice* AudioDevice)
{
    AudioDevice->StopActiveSound(Out.AudioComponentID);  // スレッドセーフ
    Out.ProceduralSound.Reset();
}
```

**注意**: AddOutput のコード例も `TSharedRef` + `MoveTemp` に更新が必要（上記メインボディの AddOutput セクション）。

### 10. QueueAudio のデータフォーマット [問題なし]

**調査結果**: USoundWaveProcedural はデフォルトで int16 を期待する。特別な設定は不要。

- `USoundWave::GetGeneratedPCMDataFormat()` のデフォルト戻り値は `EAudioMixerStreamDataFormat::Int16`
- `QueueAudio()` 内で `SampleByteSize = 2` が自動設定される（BufferSize が 2 の倍数であることを検証）
- AudioMixer が `GeneratePCMData` で int16 バイト列を受け取り、`Audio::ArrayPcm16ToFloat()` で
  float32 に自動変換する（int16 / 32767.0f）
- SDK の int16 interleaved PCM をそのまま `reinterpret_cast<const uint8*>()` で渡せばよい

### 11. OnSourceBuffer のスレッド安全性 [問題なし]

**調査結果**: SDK コールバックは単一の ASIO スレッド ("Link Main") からシリアルに呼ばれる。

- SDK は起動時に専用の "Link Main" スレッドを作成（`Context.hpp`、ASIO `io_context.run()` ループ）
- UDP 受信 → MainProcessor → SourceProcessor → Source::callback() はすべてこの ASIO スレッド上で実行
- ASIO はハンドラをシリアルに実行するため、同一 Source に対するリエントラント呼び出しは発生しない
- `Source::callback()` は `util::Locked<Callback>` (mutex) で保護されているが、これは
  `setCallback()` との競合防止用であり、コールバック自体の並行実行ではない

**結論**: `ResampleBuffer` のメンバ変数使用は安全。排他制御は不要。
`QueueAudio()` 自体も内部 TQueue で スレッドセーフなので、ASIO スレッド → オーディオスレッドの
データ受け渡しも問題ない。

### 12. QueueAudio のバッファリングレイテンシ [把握済み]

**調査結果**: UE オーディオミキサーの構造上、約 2〜3 コールバックサイクル分の遅延が発生する。

**UE 側のレイテンシ内訳** (48kHz, デフォルト設定):

| 段階 | 遅延 | 備考 |
|------|------|------|
| QueueAudio → GeneratePCMData | 0〜21.3ms | 次のミキサーコールバックまでの待ち（最悪 1 サイクル） |
| ミキサーコールバック 1 回分 | 21.3ms | CallbackBufferFrameSize = 1024 frames @ 48kHz |
| プラットフォーム出力バッファ | 21.3ms × 2 | NumBuffers = 2（XAudio2 のダブルバッファ） |

**推定合計**: 約 **43〜64ms**（UE パイプラインのみ）

**内部バッファリング**:
- `QueuedAudio` (TQueue): ロックフリー FIFO。`PumpQueuedAudio()` で一括デキュー（追加遅延なし）
- `AudioBuffer`: ワーキングバッファ。GeneratePCMData で必要分だけ memcpy
- CallbackBufferFrameSize はエンジン設定 `AudioCallbackBufferFrameSize` で変更可能（最小 240 frames）

**Link Audio 側の既存レイテンシ**:
- ネットワーク (UDP): 〜1ms（LAN 環境）
- SDK エンコード/デコード: 〜1ms
- SDK 1ms プロセスタイマー: 最大 1ms の追加待ち

**結論**: 合計 **45〜67ms** 程度。リアルタイム演奏のモニタリングには遅い（DAW 間の直接接続は
通常 5〜10ms）が、Link4UE の用途（UE 内のゲームプレイ同期、BGM ストリーミング）には許容範囲。
厳密な低レイテンシが必要な場合は `AudioCallbackBufferFrameSize` を 256 に下げれば
UE 側の遅延を約 1/4 に削減可能。

---

## 削除されるもの

- `FLink4UEAudioReceive::AudioBus` (TSoftObjectPtr<UAudioBus>)
- `#include "AudioBusSubsystem.h"` — 不要になる
- `UAudioBusSubsystem` 関連のコード (StartAudioBus, AddPatchInputForAudioBus)
- `Audio::FPatchInput` 関連のコード
- ReceiveBridge 内のチャンネルリマッピングロジック
  - USoundWaveProcedural に正しい ch 数を設定すれば UE ミキサーが処理する

---

## 実装フェーズ

### Phase 1: Settings 層の変更（コンパイル通るまで）

1. `Link4UESettings.h`: `FLink4UEAudioReceive` の `TSoftObjectPtr<UAudioBus> AudioBus` → `TSoftObjectPtr<USoundSubmix> Submix` に変更、include 更新
2. `Link4UESettingsCustomization.cpp`: プロパティハンドル名 `"AudioBus"` → `"Submix"`
3. `Link4UESubsystem.cpp`: `RebuildAudioReceives` 内の AudioBus 参照を Submix に差し替え（この時点では旧 ReceiveBridge のまま、コンパイルエラーを解消するだけ）
4. ビルド確認
5. **git commit** `"refactor(receive): replace AudioBus with Submix in settings"`

### Phase 2: ReceiveBridge 書き換え（コア実装）

6. `Link4UEReceiveBridge.h/cpp` を新設計に書き換え:
   - `FOutput` 構造体（`TStrongObjectPtr<USoundWaveProcedural>` + `AudioComponentID`）
   - `AddOutput()`: ProceduralSound 作成 → `TSharedRef<FActiveSound>` → `AddNewActiveSound`
   - `OnSourceBuffer()`: リサンプリング（線形補間）→ 全出力先に `QueueAudio`
   - デストラクタ: LinkAudioSource 破棄 → `StopActiveSound(ID)` → `TStrongObjectPtr::Reset()`
7. `RebuildAudioReceives` 更新: ChannelName でグルーピング → 1 Bridge per channel → `AddOutput` で出力先追加
8. `TearDownReceives` 更新: 破棄順序の保証（#8）
9. ビルド確認
10. **git commit** `"feat(receive): rewrite ReceiveBridge with USoundWaveProcedural pipeline"`

### Phase 3: クリーンアップ

11. 旧コード削除: `AudioBusSubsystem` 関連の include/コード、`FPatchInput` 関連、チャンネルリマッピングロジック
12. `DefaultGame.ini`: 既存設定があればプロパティ名を手動移行
13. ビルド確認
14. **git commit** `"refactor(receive): remove AudioBus/PatchInput legacy code"`
15. **git push**

### Phase 4: 結合テスト

16. Ableton Live → UE の end-to-end 音声受信テスト
17. 複数 Receive → 同一チャンネルの動作確認
18. Hot Reload（設定変更 → 再構築）の動作確認
19. エディタ上の Submix ピッカー表示確認
20. 問題があれば修正 → **git commit + push**

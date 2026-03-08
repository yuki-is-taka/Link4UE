# Audio Pipeline Redesign — SoundWaveProcedural 方式

> Status: **Draft** — 設計分析フェーズ
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

```cpp
class FLink4UEReceiveBridge
{
public:
    FLink4UEReceiveBridge(ableton::LinkAudio& InLink,
                          const ableton::ChannelId& InChannelId,
                          int32 InNumChannels,
                          int32 InSampleRate,
                          USoundSubmix* InTargetSubmix,  // nullptr = Master
                          FAudioDevice* InAudioDevice);
    ~FLink4UEReceiveBridge();

private:
    void OnSourceBuffer(ableton::LinkAudioSource::BufferHandle Handle);

    ableton::LinkAudioSource Source;

    TStrongObjectPtr<USoundWaveProcedural> ProceduralSound;  // GC 防止
    FActiveSound* ActiveSound;  // FAudioDevice が所有、Bridge はポインタのみ

    int32 DeviceSampleRate;
    TArray<int16> ResampleBuffer;  // リサンプリング用ワークバッファ

    FString ChannelName;
};
```

### 初期化

```cpp
void FLink4UEReceiveBridge::Initialize(FAudioDevice* AudioDevice,
                                        USoundSubmix* TargetSubmix)
{
    // 1. USoundWaveProcedural 作成
    USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>();
    Wave->SetSampleRate(DeviceSampleRate);
    Wave->NumChannels = NumChannels;
    Wave->Duration = INDEFINITELY_LOOPING_DURATION;
    Wave->SoundGroup = SOUNDGROUP_Default;
    Wave->bLooping = true;
    ProceduralSound.Reset(Wave);

    // 2. FActiveSound 構築 (World/Actor/Component 不要)
    FActiveSound NewActiveSound;
    NewActiveSound.SetSound(Wave);
    NewActiveSound.SetWorld(nullptr);
    NewActiveSound.bAllowSpatialization = false;
    NewActiveSound.bIsUISound = true;  // World 非依存
    NewActiveSound.bLocationDefined = false;

    if (TargetSubmix)
    {
        // SoundSubmixSends に追加
        FSoundSubmixSendInfo SubmixSend;
        SubmixSend.SoundSubmix = TargetSubmix;
        SubmixSend.SendLevel = 1.0f;
        NewActiveSound.SetSubmixSend(SubmixSend);
    }

    AudioDevice->AddNewActiveSound(NewActiveSound);
}
```

### OnSourceBuffer コールバック

```cpp
void FLink4UEReceiveBridge::OnSourceBuffer(BufferHandle Handle)
{
    const int32 SrcRate = Handle.info.sampleRate;
    const int32 SrcFrames = Handle.info.numFrames;
    const int32 SrcChannels = Handle.info.numChannels;

    if (SrcRate == DeviceSampleRate)
    {
        // レート一致 — そのまま QueueAudio
        const int32 ByteSize = SrcFrames * SrcChannels * sizeof(int16_t);
        ProceduralSound->QueueAudio(
            reinterpret_cast<const uint8*>(Handle.samples), ByteSize);
        return;
    }

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
    ProceduralSound->QueueAudio(
        (const uint8*)ResampleBuffer.GetData(),
        DstFrames * SrcChannels * sizeof(int16));
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

### 5. Master Send (自動 Sink) との関係

現行設計では Link Audio 有効時に Master Submix の Sink を自動作成している（SDK がピアのリターンパスに Sink を要求するため）。

**新設計での変更点**:
- Master Send は引き続き必要（SDK 要件）
- Send 設定で空 = Master の場合、この自動 Sink と重複しないよう注意
- 自動 Sink と明示的 Send エントリの統合ロジックが必要

### 6. エディタ設定カスタマイズの更新

`FLink4UEAudioReceiveCustomization` を更新する必要がある:
- `AudioBus` プロパティ → `Submix` プロパティに変更
- チャンネル名ドロップダウンは現行のまま維持

### 7. 複数 Receive が同一チャンネルを参照するケース

同じ Link チャンネルを複数の Submix に送りたい場合:
- 同一チャンネルに対して複数の `LinkAudioSource` を作成することは SDK 上可能か？
- 可能でない場合、1 Source → 複数 `USoundWaveProcedural` にコピーする設計が必要

### 8. Hot Reload (エディタでの設定変更)

設定変更時の挙動:
- `OnSettingsChanged` → `RebuildAudioReceives`
- 既存の ReceiveBridge を破棄: FActiveSound の停止 → TStrongObjectPtr 解放 (自動 GC)
- TearDown 順序: Source コールバック停止 → ActiveSound 停止 → ProceduralSound 解放
- オーディオスレッドが GeneratePCMData() 中に ProceduralSound が破棄されないよう注意
- `bIsRebuilding` ガードで再帰的リビルドを防止 (既に実装済み)

---

## 削除されるもの

- `FLink4UEAudioReceive::AudioBus` (TSoftObjectPtr<UAudioBus>)
- `#include "AudioBusSubsystem.h"` — 不要になる
- `UAudioBusSubsystem` 関連のコード (StartAudioBus, AddPatchInputForAudioBus)
- `Audio::FPatchInput` 関連のコード
- ReceiveBridge 内のチャンネルリマッピングロジック
  - USoundWaveProcedural に正しい ch 数を設定すれば UE ミキサーが処理する

---

## 実装フェーズ (案)

1. **Settings 変更**: `FLink4UEAudioReceive` を AudioBus → Submix に変更
2. **ReceiveBridge 書き換え**: PatchInput → USoundWaveProcedural + FActiveSound
3. **リサンプリング実装**: OnSourceBuffer 内の線形補間リサンプラー
4. **RebuildAudioReceives 更新**: 新 Bridge の生成・破棄ロジック
5. **TearDown 更新**: Source 停止 → ActiveSound 停止 → TStrongObjectPtr 解放
6. **エディタカスタマイズ更新**: AudioBus ピッカー → Submix ピッカー
7. **DefaultGame.ini 更新**: 新プロパティ形式に移行
8. **結合テスト**: Ableton Live → UE 音声出力の end-to-end 確認

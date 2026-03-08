# Link Audio SDK — Sink とピアディスカバリの仕組み

> Date: 2026-03-08
> SDK: Ableton Link Audio 4.0.0-beta2
> Related: [audio-pipeline-redesign.md](audio-pipeline-redesign.md)

## 結論

**Sink (送信側) が存在しないと、リモートピアからの音声受信 (Source) は機能しない。**
これは SDK のピアディスカバリプロトコルの設計上の制約であり、バグではない。

---

## SDK のピアディスカバリプロトコル

### 用語整理

| SDK 用語 | 役割 | UE 側の対応 |
|----------|------|-------------|
| **Sink** | このピアの音声を**送出**する (UE → ネットワーク) | `FLink4UESendBridge` (ISubmixBufferListener) |
| **Source** | リモートピアの音声を**受信**する (ネットワーク → UE) | `FLink4UEReceiveBridge` (USoundWaveProcedural) |
| **ChannelAnnouncement** | このピアが持つ Sink の一覧をネットワークに広告する | 自動 (SDK 内部) |
| **ChannelRequest** | リモートの Source がこのピアの Sink に接続を要求する | 自動 (SDK 内部) |

### 接続確立シーケンス

```
Node A (UE)                              Node B (Ableton Live)
===========                              =====================

1. Sink "MyPeer" を作成
   |
   v
2. PeerAnnouncement を UDP ブロードキャスト
   (channelAnnouncements に Sink 情報を含む)
   ─────────────────────────────────────>
                                         3. アナウンスメントを受信
                                            "MyPeer" チャンネルを発見
                                            |
                                            v
                                         4. Source を作成、ChannelRequest 送信
   <─────────────────────────────────────
5. SinkProcessor が ChannelRequest を受信
   mReceivers に追加
   Sink.mIsConnected = true
   |
   v
6. Sink.retainBuffer() が有効になる
   → オーディオデータの送出開始
   ─────────────────────────────────────>
                                         7. Source でオーディオ受信
```

**重要**: ステップ 2 の `channelAnnouncements()` が空の場合、Node B は Node A のチャンネルを
発見できないため、ステップ 4 以降が起きない。

---

## SDK ソースコードの根拠

### channelAnnouncements() は Sink からのみ構築される

**`MainProcessor.hpp:191-200`**:
```cpp
ChannelAnnouncements channelAnnouncements() const
{
  auto announcements = ChannelAnnouncements{};
  for (auto& sink : mSinks)
  {
    announcements.channels.emplace_back(
      ChannelAnnouncement{sink->name(), sink->id()});
  }
  return announcements;
}
```

Source は `channelAnnouncements` に含まれない。Sink のみが広告される。

### Sink は接続されるまでバッファを確保しない

**`Sink.hpp:58-79`**:
```cpp
Buffer<int16_t>* retainBuffer()
{
  auto queueWriter = mQueue.writer();
  if (!mIsConnected || queueWriter.numRetainedSlots() > 0)
  {
    return nullptr;  // 未接続ならバッファなし
  }
  // ... バッファ確保
}
```

`mIsConnected` は `SinkProcessor::receiveChannelRequest()` で true に設定される。
つまり、リモートの Source が ChannelRequest を送るまで Sink は実質的に休眠状態。

### SinkProcessor はレシーバーが空なら送信しない

**`SinkProcessor.hpp:105-107`**:
```cpp
if (!mReceivers.empty() && mQueueReader[0]->mTempo > link::Tempo{0})
{
  mEncoder(*mQueueReader[0]);  // レシーバーが存在するときのみエンコード
}
```

### PeerAnnouncement に channelAnnouncements が含まれる

**`Controller.hpp:174`** (updateAudioDiscovery 内):
```cpp
mGateways.updateAnnouncement(PeerAnnouncement{
  this->mNodeId, this->mSessionId, mPeerInfo, mProcessor.channelAnnouncements()});
```

---

## なぜ「Sink がないと Source (受信) も動かない」のか

一見すると矛盾しているように見える — Source (受信) が欲しいのに、なぜ Sink (送信) が必要なのか？

### SDK の設計思想

Link Audio のプロトコルは **双方向チャンネル** を前提としている:

1. ピア A が Sink を作成 → チャンネル "X" をネットワークに広告
2. ピア B がチャンネル "X" を発見 → Source を作成してピア A に接続要求
3. ピア A は **接続されたことを検知** → 音声送出を開始
4. ピア B は音声を受信

**ピア A (UE) の視点**:
- Sink = 「このチャンネルを持っている」というネットワーク上の宣言
- Source = 「あのチャンネルのデータが欲しい」というリクエスト

**Sink なしだと**:
- UE は何もアナウンスしない → Ableton Live から UE のチャンネルが見えない
- Live が UE にオーディオを送る宛先がない
- `channelAnnouncements()` が空のため、Live 側で UE のチャンネルを選択する UI にも表示されない

### つまり

Sink は「送信のため」だけでなく、**ネットワーク上でこのピアの存在を可視化するため** に必要。
Sink がない Link Audio ピアは、ネットワーク上では「オーディオ機能を持たないピア」として扱われる。

---

## Link4UE の現行実装: Master Sink の自動作成

### 実装箇所

**`Link4UESubsystem.cpp:568-586`** (`RebuildAudioSends` 内):

```cpp
// Always create a Sink for the master submix output.
{
    USoundSubmix& MasterSubmix = AudioDevice->GetMainSubmixObject();
    FString MasterChannelName = Settings->PeerName;

    TSharedRef<FLink4UESendBridge, ESPMode::ThreadSafe> Bridge =
        MakeShared<FLink4UESendBridge, ESPMode::ThreadSafe>(
            LinkInstance->Link, MasterChannelName, kDefaultMaxSamples, Quantum);

    AudioDevice->RegisterSubmixBufferListener(Bridge, MasterSubmix);

    LinkInstance->MasterSend.Bridge = Bridge;
    LinkInstance->MasterSend.Submix = &MasterSubmix;
}
```

### 動作条件

| 条件 | 詳細 |
|------|------|
| `bEnableLinkAudio == true` | Link Audio が有効であること |
| `GetMainAudioDevice() != nullptr` | AudioDevice が利用可能であること |

AudioDevice が未初期化の場合、`bSendRoutesPending = true` にして遅延再試行する。

### ユーザー設定の Send との関係

```
RebuildAudioSends()
    |
    +-- Master Sink (無条件作成)
    |       チャンネル名 = Settings->PeerName
    |       Submix = GetMainSubmixObject()
    |       格納先 = LinkInstance->MasterSend
    |
    +-- ユーザー設定 Sends (AudioSends 配列から作成)
            チャンネル名 = ChannelNamePrefix (空の場合 Submix 名)
            Submix = ユーザー指定 (空 = Master)
            格納先 = LinkInstance->ActiveSends[]
```

- Master Sink は **常に最初に作成** される
- ユーザーが `AudioSends` を一つも設定していなくても、Master Sink は作成される
- これにより、最低限のネットワーク可視性が保証される

### Master Sink が送出する音声

Master Submix の `ISubmixBufferListener` として登録されるため、**UE の最終ミックス出力** がキャプチャされ、
Link Audio ネットワーク上のチャンネルとして送出される。

これは意図的な設計:
- UE の音をリモートピア (DAW 等) でモニターできる
- Receive で受信した音が Master Submix を通る場合、**フィードバックループのリスク** がある
  → Receive の出力先を Master 以外の Submix に設定するか、Send 側で別の Submix を使うことで回避

---

## 新パイプライン設計への影響

パイプライン再設計 (USoundWaveProcedural 方式) においても:

1. **Master Sink の自動作成は維持する** — SDK 要件のため廃止不可
2. **Send 側のコードは変更不要** — ISubmixBufferListener ベースの既存実装がそのまま使える
3. **Receive (Source) は Sink の存在に依存する** ことを UI/ドキュメントで明示すべき
4. **フィードバック防止**: Receive の出力 Submix と Master Sink のキャプチャ Submix が
   同じ場合にフィードバックが起きる。設定 UI で警告するか、Submix 分離を推奨する

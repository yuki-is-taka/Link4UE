# Link Audio SDK — Sink and Peer Discovery Mechanics

> Date: 2026-03-08
> SDK: Ableton Link Audio 4.0.0-beta2
> Related: [audio-pipeline-redesign.md](audio-pipeline-redesign.md)

## Conclusion

**Without a Sink (send side), audio reception via Source does not work.**
This is a design constraint of the SDK's peer discovery protocol, not a bug.

---

## SDK Peer Discovery Protocol

### Terminology

| SDK Term | Role | UE Counterpart |
|----------|------|----------------|
| **Sink** | **Sends** this peer's audio to the network (UE → network) | `FLink4UESendBridge` (ISubmixBufferListener) |
| **Source** | **Receives** a remote peer's audio from the network (network → UE) | `FLink4UEReceiveBridge` (USoundWaveProcedural) |
| **ChannelAnnouncement** | Advertises this peer's Sinks to the network | Automatic (SDK internal) |
| **ChannelRequest** | A remote Source requests connection to this peer's Sink | Automatic (SDK internal) |

### Connection Establishment Sequence

```
Node A (UE)                              Node B (Ableton Live)
===========                              =====================

1. Create Sink "Main"
   |
   v
2. UDP broadcast PeerAnnouncement
   (includes Sink info in channelAnnouncements)
   ─────────────────────────────────────>
                                         3. Receive announcement
                                            Discover "Main" channel
                                            |
                                            v
                                         4. Create Source, send ChannelRequest
   <─────────────────────────────────────
5. SinkProcessor receives ChannelRequest
   Add to mReceivers
   Sink.mIsConnected = true
   |
   v
6. Sink.retainBuffer() becomes active
   → Audio data transmission begins
   ─────────────────────────────────────>
                                         7. Source receives audio
```

**Critical**: If `channelAnnouncements()` is empty at step 2, Node B cannot discover Node A's channels, so steps 4 onward never occur.

---

## SDK Source Code Evidence

### channelAnnouncements() is built from Sinks only

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

Sources are not included in `channelAnnouncements`. Only Sinks are advertised.

### Sinks do not allocate buffers until connected

**`Sink.hpp:58-79`**:
```cpp
Buffer<int16_t>* retainBuffer()
{
  auto queueWriter = mQueue.writer();
  if (!mIsConnected || queueWriter.numRetainedSlots() > 0)
  {
    return nullptr;  // No buffer if not connected
  }
  // ... buffer allocation
}
```

`mIsConnected` is set to true by `SinkProcessor::receiveChannelRequest()`. The Sink remains dormant until a remote Source sends a ChannelRequest.

### SinkProcessor does not transmit without receivers

**`SinkProcessor.hpp:105-107`**:
```cpp
if (!mReceivers.empty() && mQueueReader[0]->mTempo > link::Tempo{0})
{
  mEncoder(*mQueueReader[0]);  // Encode only when receivers exist
}
```

### PeerAnnouncement includes channelAnnouncements

**`Controller.hpp:174`** (inside updateAudioDiscovery):
```cpp
mGateways.updateAnnouncement(PeerAnnouncement{
  this->mNodeId, this->mSessionId, mPeerInfo, mProcessor.channelAnnouncements()});
```

---

## Why "No Sink Means No Source (Receive)"

At first glance this seems contradictory — you want a Source (receive), so why is a Sink (send) required?

### SDK Design Philosophy

The Link Audio protocol assumes **bidirectional channels**:

1. Peer A creates a Sink → advertises channel "X" on the network
2. Peer B discovers channel "X" → creates a Source and sends a connection request to Peer A
3. Peer A **detects the connection** → begins transmitting audio
4. Peer B receives the audio

**From Peer A's (UE) perspective**:
- Sink = a declaration on the network: "I have this channel"
- Source = a request: "I want data from that channel"

**Without a Sink**:
- UE announces nothing → Ableton Live cannot see any UE channels
- Live has no destination to send audio to
- `channelAnnouncements()` is empty, so UE's channels do not appear in Live's channel selection UI

### In Short

A Sink is needed not just "for sending" but **to make this peer visible on the network as an audio-capable participant**. A Link Audio peer without any Sinks is treated as "a peer with no audio capabilities" by the network.

---

## Link4UE Implementation: Auto Master Sink

### Implementation Location

**`Link4UESubsystem.cpp`** (inside `RebuildAudioSends`):

```cpp
// Ensure master send exists (required by SDK for peers to establish audio return paths).
if (!LinkInstance->MasterSend.Bridge.IsValid())
{
    USoundSubmix& MasterSubmix = AudioDevice->GetMainSubmixObject();
    FString MasterChannelName = TEXT("Main");

    TSharedRef<FLink4UESendBridge, ESPMode::ThreadSafe> Bridge =
        MakeShared<FLink4UESendBridge, ESPMode::ThreadSafe>(
            LinkInstance->Link, MasterChannelName, kDefaultMaxSamples, Quantum);

    AudioDevice->RegisterSubmixBufferListener(Bridge, MasterSubmix);

    LinkInstance->MasterSend.Bridge = Bridge;
    LinkInstance->MasterSend.Submix = &MasterSubmix;
}
```

### Activation Conditions

| Condition | Details |
|-----------|---------|
| `bEnableLinkAudio == true` | Link Audio must be enabled |
| `GetMainAudioDevice() != nullptr` | AudioDevice must be available |

If AudioDevice is not initialized, `bSendRoutesPending = true` triggers deferred retry.

### Relationship to User-Configured Sends

```
RebuildAudioSends()
    |
    +-- Master Sink (unconditional, preserved across rebuilds)
    |       Channel name = "Main"
    |       Submix = GetMainSubmixObject()
    |       Stored in = LinkInstance->MasterSend
    |
    +-- User Sends (created from AudioSends array, diff-based)
            Channel name = ChannelNamePrefix (or Submix name if empty)
            Submix = user-specified (empty = Master)
            Stored in = LinkInstance->ActiveSends[]
```

- Master Sink is **always created first** and preserved across user send changes
- Even with zero user-configured `AudioSends`, the Master Sink is created
- This guarantees minimum network visibility

### Audio Transmitted by Master Sink

Registered as an `ISubmixBufferListener` on the Master Submix, it captures **UE's final mix output** and transmits it as a Link Audio channel.

This is intentional:
- UE's audio can be monitored in a remote peer (DAW, etc.)
- If Receive output targets the Master Submix, there is a **feedback loop risk** → route Receive output to a different Submix, or use a different Submix for sending

---

## Impact on New Pipeline Design

With the USoundWaveProcedural pipeline:

1. **Auto Master Sink creation is maintained** — cannot be removed (SDK requirement)
2. **Send-side code is unchanged** — existing ISubmixBufferListener implementation works as-is
3. **Receive (Source) depends on Sink existence** — this should be made clear in UI/documentation
4. **Feedback prevention**: If a Receive's output Submix is the same as the Master Sink's capture Submix, feedback occurs. The settings UI should warn about this or recommend Submix separation

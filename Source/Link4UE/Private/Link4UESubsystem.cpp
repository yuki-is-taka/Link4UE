// Copyright Tokyologist. All Rights Reserved.

#include "Link4UESubsystem.h"
#include "Link4UESettings.h"
#include "ableton/LinkAudio.hpp"

namespace
{
	FString NodeIdToHex(const ableton::link::NodeId& Id)
	{
		FString Out;
		Out.Reserve(16);
		for (uint8 Byte : Id)
		{
			Out += FString::Printf(TEXT("%02x"), Byte);
		}
		return Out;
	}
}

DEFINE_LOG_CATEGORY_STATIC(LogLink4UE, Log, All);

// ---------------------------------------------------------------------------
// Pimpl — hides ableton::LinkAudio from the header
// ---------------------------------------------------------------------------
struct ULink4UESubsystem::FLinkInstance
{
	ableton::LinkAudio Link;

	FLinkInstance(double BPM, const std::string& PeerName)
		: Link(BPM, PeerName)
	{
	}
};

// ---------------------------------------------------------------------------
// Constructor / Destructor — defined here so FLinkInstance is complete
// ---------------------------------------------------------------------------

ULink4UESubsystem::ULink4UESubsystem() = default;

ULink4UESubsystem::~ULink4UESubsystem()
{
	delete LinkInstance;
	LinkInstance = nullptr;
}

// ---------------------------------------------------------------------------
// USubsystem interface
// ---------------------------------------------------------------------------

void ULink4UESubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Read settings
	const ULink4UESettings* Settings = GetDefault<ULink4UESettings>();
	const double QuantumBeats = Link4UEQuantumToBeats(Settings->DefaultQuantum);
	Snapshot.Tempo = Settings->DefaultTempo;
	Snapshot.Quantum = QuantumBeats;
	Quantum.store(QuantumBeats, std::memory_order_relaxed);

	LinkInstance = new FLinkInstance(Settings->DefaultTempo,
		TCHAR_TO_UTF8(*Settings->PeerName));

	// Register Link-thread callbacks — store values atomically, consume on GameThread
	LinkInstance->Link.setNumPeersCallback([this](std::size_t InNumPeers)
	{
		PendingNumPeers.store(static_cast<int32>(InNumPeers), std::memory_order_relaxed);
		bNumPeersDirty.store(true, std::memory_order_release);
	});

	LinkInstance->Link.setTempoCallback([this](double InBPM)
	{
		PendingTempo.store(InBPM, std::memory_order_relaxed);
		bTempoDirty.store(true, std::memory_order_release);
	});

	LinkInstance->Link.setStartStopCallback([this](bool bInIsPlaying)
	{
		PendingIsPlaying.store(bInIsPlaying, std::memory_order_relaxed);
		bStartStopDirty.store(true, std::memory_order_release);
	});

	// Tick on GameThread via FTSTicker
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &ULink4UESubsystem::Tick));

	// LinkAudio channels-changed callback
	LinkInstance->Link.setChannelsChangedCallback([this]()
	{
		bChannelsDirty.store(true, std::memory_order_release);
	});

	// Apply settings
	LinkInstance->Link.enableStartStopSync(Settings->bStartStopSync);
	if (Settings->bEnableLinkAudio)
	{
		LinkInstance->Link.enableLinkAudio(true);
	}
	if (Settings->bAutoConnect)
	{
		LinkInstance->Link.enable(true);
	}

	UE_LOG(LogLink4UE, Log, TEXT("Link4UE subsystem initialized (tempo=%.1f, auto=%s, audio=%s)"),
		Settings->DefaultTempo,
		Settings->bAutoConnect ? TEXT("on") : TEXT("off"),
		Settings->bEnableLinkAudio ? TEXT("on") : TEXT("off"));
}

void ULink4UESubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);

	if (LinkInstance)
	{
		LinkInstance->Link.enableLinkAudio(false);
		LinkInstance->Link.enable(false);
		delete LinkInstance;
		LinkInstance = nullptr;
	}

	UE_LOG(LogLink4UE, Log, TEXT("Link4UE subsystem deinitialized"));
	Super::Deinitialize();
}

// ---------------------------------------------------------------------------
// Tick — capture session state & dispatch delegates
// ---------------------------------------------------------------------------

bool ULink4UESubsystem::Tick(float DeltaTime)
{
	if (!LinkInstance)
	{
		return true; // keep ticking
	}

	const double Q = Quantum.load(std::memory_order_relaxed);
	const auto Now = LinkInstance->Link.clock().micros();
	const auto State = LinkInstance->Link.captureAppSessionState();

	Snapshot.Tempo = State.tempo();
	Snapshot.Beat = State.beatAtTime(Now, Q);
	Snapshot.Phase = State.phaseAtTime(Now, Q);
	Snapshot.bIsPlaying = State.isPlaying();
	Snapshot.NumPeers = static_cast<int32>(LinkInstance->Link.numPeers());
	Snapshot.Quantum = Q;

	// Beat / Phase-zero edge detection
	const int32 CurBeatFloor = FMath::FloorToInt32(Snapshot.Beat);
	if (PrevBeatFloor >= 0 && CurBeatFloor > PrevBeatFloor)
	{
		// Fire for each beat crossed (handles multiple beats per frame)
		for (int32 B = PrevBeatFloor + 1; B <= CurBeatFloor; ++B)
		{
			const bool bPhaseZero = FMath::IsNearlyZero(FMath::Fmod(static_cast<double>(B), Q), 0.001);
			OnBeat.Broadcast(B, bPhaseZero);
			if (bPhaseZero)
			{
				OnPhaseZero.Broadcast();
			}
		}
	}
	PrevBeatFloor = CurBeatFloor;

	// Dispatch callbacks accumulated from Link thread
	if (bNumPeersDirty.exchange(false, std::memory_order_acquire))
	{
		OnNumPeersChanged.Broadcast(PendingNumPeers.load(std::memory_order_relaxed));
	}
	if (bTempoDirty.exchange(false, std::memory_order_acquire))
	{
		OnTempoChanged.Broadcast(PendingTempo.load(std::memory_order_relaxed));
	}
	if (bStartStopDirty.exchange(false, std::memory_order_acquire))
	{
		OnStartStopChanged.Broadcast(PendingIsPlaying.load(std::memory_order_relaxed));
	}
	if (bChannelsDirty.exchange(false, std::memory_order_acquire))
	{
		OnChannelsChanged.Broadcast();
	}

	return true; // keep ticking
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

double ULink4UESubsystem::GetTimeAtBeat(double Beat) const
{
	if (!LinkInstance) return 0.0;

	const auto State = LinkInstance->Link.captureAppSessionState();
	const double Q = Quantum.load(std::memory_order_relaxed);
	const auto Micros = State.timeAtBeat(Beat, Q);
	return std::chrono::duration<double>(Micros).count();
}

// ---------------------------------------------------------------------------
// Mutators
// ---------------------------------------------------------------------------

void ULink4UESubsystem::SetTempo(double BPM)
{
	if (!LinkInstance) return;

	auto State = LinkInstance->Link.captureAppSessionState();
	State.setTempo(BPM, LinkInstance->Link.clock().micros());
	LinkInstance->Link.commitAppSessionState(State);
}

void ULink4UESubsystem::SetQuantum(double InQuantum)
{
	Quantum.store(InQuantum, std::memory_order_relaxed);
}

void ULink4UESubsystem::SetQuantumPreset(ELink4UEQuantum Preset)
{
	Quantum.store(Link4UEQuantumToBeats(Preset), std::memory_order_relaxed);
}

void ULink4UESubsystem::SetIsPlaying(bool bPlay)
{
	if (!LinkInstance) return;

	auto State = LinkInstance->Link.captureAppSessionState();
	const auto Now = LinkInstance->Link.clock().micros();
	const double Q = Quantum.load(std::memory_order_relaxed);

	if (bPlay)
	{
		State.setIsPlayingAndRequestBeatAtTime(true, Now, 0.0, Q);
	}
	else
	{
		State.setIsPlaying(false, Now);
	}
	LinkInstance->Link.commitAppSessionState(State);
}

void ULink4UESubsystem::RequestBeatAtTime(double Beat)
{
	if (!LinkInstance) return;

	auto State = LinkInstance->Link.captureAppSessionState();
	const auto Now = LinkInstance->Link.clock().micros();
	const double Q = Quantum.load(std::memory_order_relaxed);

	State.requestBeatAtTime(Beat, Now, Q);
	LinkInstance->Link.commitAppSessionState(State);
}

// ---------------------------------------------------------------------------
// Enable / Disable
// ---------------------------------------------------------------------------

void ULink4UESubsystem::EnableLink()
{
	if (LinkInstance)
	{
		LinkInstance->Link.enable(true);
		UE_LOG(LogLink4UE, Log, TEXT("Link enabled"));
	}
}

void ULink4UESubsystem::DisableLink()
{
	if (LinkInstance)
	{
		LinkInstance->Link.enable(false);
		UE_LOG(LogLink4UE, Log, TEXT("Link disabled"));
	}
}

bool ULink4UESubsystem::IsLinkEnabled() const
{
	return LinkInstance && LinkInstance->Link.isEnabled();
}

void ULink4UESubsystem::EnableStartStopSync(bool bEnable)
{
	if (LinkInstance)
	{
		LinkInstance->Link.enableStartStopSync(bEnable);
	}
}

bool ULink4UESubsystem::IsStartStopSyncEnabled() const
{
	return LinkInstance && LinkInstance->Link.isStartStopSyncEnabled();
}

// ---------------------------------------------------------------------------
// LinkAudio
// ---------------------------------------------------------------------------

void ULink4UESubsystem::EnableLinkAudio(bool bEnable)
{
	if (LinkInstance)
	{
		LinkInstance->Link.enableLinkAudio(bEnable);
		UE_LOG(LogLink4UE, Log, TEXT("Link Audio %s"), bEnable ? TEXT("enabled") : TEXT("disabled"));
	}
}

bool ULink4UESubsystem::IsLinkAudioEnabled() const
{
	return LinkInstance && LinkInstance->Link.isLinkAudioEnabled();
}

void ULink4UESubsystem::SetPeerName(const FString& InPeerName)
{
	if (LinkInstance)
	{
		LinkInstance->Link.setPeerName(TCHAR_TO_UTF8(*InPeerName));
	}
}

TArray<FLink4UEChannel> ULink4UESubsystem::GetChannels() const
{
	TArray<FLink4UEChannel> Result;
	if (!LinkInstance)
	{
		return Result;
	}

	const auto Channels = LinkInstance->Link.channels();
	Result.Reserve(static_cast<int32>(Channels.size()));

	for (const auto& Ch : Channels)
	{
		FLink4UEChannel& Out = Result.AddDefaulted_GetRef();
		Out.ChannelId = NodeIdToHex(Ch.id);
		Out.Name = UTF8_TO_TCHAR(Ch.name.c_str());
		Out.PeerId = NodeIdToHex(Ch.peerId);
		Out.PeerName = UTF8_TO_TCHAR(Ch.peerName.c_str());
	}

	return Result;
}

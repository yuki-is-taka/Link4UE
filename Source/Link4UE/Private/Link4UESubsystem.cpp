// Copyright Tokyologist. All Rights Reserved.

#include "Link4UESubsystem.h"
#include "ableton/Link.hpp"

DEFINE_LOG_CATEGORY_STATIC(LogLink4UE, Log, All);

// ---------------------------------------------------------------------------
// Pimpl — hides ableton::Link from the header
// ---------------------------------------------------------------------------
struct ULink4UESubsystem::FLinkInstance
{
	ableton::Link Link;

	explicit FLinkInstance(double BPM)
		: Link(BPM)
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

	LinkInstance = new FLinkInstance(Snapshot.Tempo);

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

	UE_LOG(LogLink4UE, Log, TEXT("Link4UE subsystem initialized (tempo=%.1f)"), Snapshot.Tempo);
}

void ULink4UESubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);

	if (LinkInstance)
	{
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

	return true; // keep ticking
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

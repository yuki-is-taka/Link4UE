// Copyright YUKITAKA. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "AudioDeviceHandle.h"
#include "Link4UETypes.h"
#include "Link4UESubsystem.generated.h"

/** Snapshot of Link session state, captured once per frame. */
USTRUCT(BlueprintType)
struct LINK4UE_API FLink4UESessionSnapshot
{
	GENERATED_BODY()

	/** Current tempo in BPM. */
	UPROPERTY(BlueprintReadOnly, Category = "Link4UE")
	double Tempo = 120.0;

	/** Beat position on the shared timeline. */
	UPROPERTY(BlueprintReadOnly, Category = "Link4UE")
	double Beat = 0.0;

	/** Phase within the current quantum [0, Quantum). */
	UPROPERTY(BlueprintReadOnly, Category = "Link4UE")
	double Phase = 0.0;

	/** Whether transport is playing. */
	UPROPERTY(BlueprintReadOnly, Category = "Link4UE")
	bool bIsPlaying = false;

	/** Number of peers in the session (excluding self). */
	UPROPERTY(BlueprintReadOnly, Category = "Link4UE")
	int32 NumPeers = 0;

	/** Quantum used for beat/phase calculation. */
	UPROPERTY(BlueprintReadOnly, Category = "Link4UE")
	double Quantum = 4.0;
};

/** Describes an audio channel visible in the Link Audio session. */
USTRUCT(BlueprintType)
struct LINK4UE_API FLink4UEChannel
{
	GENERATED_BODY()

	/** Unique channel identifier (hex string). */
	UPROPERTY(BlueprintReadOnly, Category = "Link4UE")
	FString ChannelId;

	/** Display name of the channel. */
	UPROPERTY(BlueprintReadOnly, Category = "Link4UE")
	FString Name;

	/** Unique peer identifier (hex string). */
	UPROPERTY(BlueprintReadOnly, Category = "Link4UE")
	FString PeerId;

	/** Display name of the peer providing the channel. */
	UPROPERTY(BlueprintReadOnly, Category = "Link4UE")
	FString PeerName;
};

// Delegates — fired on GameThread
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLink4UEOnNumPeersChanged, int32, NumPeers);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLink4UEOnTempoChanged, double, BPM);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLink4UEOnStartStopChanged, bool, bIsPlaying);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FLink4UEOnBeat, int32, BeatNumber, bool, bIsPhaseZero);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FLink4UEOnPhaseZero);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FLink4UEOnChannelsChanged);

/**
 * Engine-lifetime subsystem that owns the Ableton Link instance.
 * One per process — safe across PIE sessions and editor use.
 */
UCLASS(Config = EditorPerProjectUserSettings)
class LINK4UE_API ULink4UESubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	ULink4UESubsystem();
	virtual ~ULink4UESubsystem() override;

	// USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	// --- Connection ---

	UPROPERTY(Config, EditAnywhere, Category = "Connection", meta = (
		DisplayName = "Auto Connect"))
	bool bAutoConnect = true;

	UPROPERTY(Config, EditAnywhere, Category = "Connection", meta = (
		DisplayName = "Start/Stop Sync"))
	bool bStartStopSync = true;

	UPROPERTY(Config, EditAnywhere, Category = "Connection", meta = (
		DisplayName = "Enable Link Audio"))
	bool bEnableLinkAudio = false;

	UPROPERTY(Config, EditAnywhere, Category = "Connection", meta = (
		DisplayName = "Peer Name"))
	FString PeerName = TEXT("Unreal");

	// --- Defaults ---

	UPROPERTY(Config, EditAnywhere, Category = "Defaults", meta = (
		DisplayName = "Default Tempo (BPM)",
		ClampMin = "20.0", ClampMax = "999.0"))
	double DefaultTempo = 120.0;

	UPROPERTY(Config, EditAnywhere, Category = "Defaults", meta = (
		DisplayName = "Default Quantum"))
	ELink4UEQuantum DefaultQuantum = ELink4UEQuantum::Bar_1;

	// --- Audio Routing ---

	UPROPERTY(Config, EditAnywhere, Category = "Audio Routing", meta = (
		DisplayName = "Audio Sends"))
	TArray<FLink4UEAudioSend> AudioSends;

	UPROPERTY(Config, EditAnywhere, Category = "Audio Routing", meta = (
		DisplayName = "Audio Receives"))
	TArray<FLink4UEAudioReceive> AudioReceives;

	// --- Query (GameThread) ---

	/** Returns the most recent session snapshot (updated each tick). */
	UFUNCTION(BlueprintCallable, Category = "Link4UE")
	const FLink4UESessionSnapshot& GetSessionSnapshot() const { return Snapshot; }

	/** Returns the time (in seconds) at which the given beat will occur.
	 *  Useful for scheduling events ahead of time (e.g. "next bar starts in X seconds"). */
	UFUNCTION(BlueprintCallable, Category = "Link4UE")
	double GetTimeAtBeat(double Beat) const;

	// --- Mutators (GameThread, thread-safe via Link API) ---

	UFUNCTION(BlueprintCallable, Category = "Link4UE")
	void SetTempo(double BPM);

	UFUNCTION(BlueprintCallable, Category = "Link4UE")
	void SetQuantum(double InQuantum);

	UFUNCTION(BlueprintCallable, Category = "Link4UE", meta = (DisplayName = "Set Quantum (Preset)"))
	void SetQuantumPreset(ELink4UEQuantum Preset);

	UFUNCTION(BlueprintCallable, Category = "Link4UE")
	void SetIsPlaying(bool bPlay);

	UFUNCTION(BlueprintCallable, Category = "Link4UE")
	void RequestBeatAtTime(double Beat);

	// --- Enable / Disable ---

	UFUNCTION(BlueprintCallable, Category = "Link4UE")
	void EnableLink();

	UFUNCTION(BlueprintCallable, Category = "Link4UE")
	void DisableLink();

	UFUNCTION(BlueprintCallable, Category = "Link4UE")
	bool IsLinkEnabled() const;

	UFUNCTION(BlueprintCallable, Category = "Link4UE")
	void EnableStartStopSync(bool bEnable);

	UFUNCTION(BlueprintCallable, Category = "Link4UE")
	bool IsStartStopSyncEnabled() const;

	// --- LinkAudio ---

	UFUNCTION(BlueprintCallable, Category = "Link4UE|Audio")
	void EnableLinkAudio(bool bEnable);

	UFUNCTION(BlueprintCallable, Category = "Link4UE|Audio")
	bool IsLinkAudioEnabled() const;

	UFUNCTION(BlueprintCallable, Category = "Link4UE|Audio")
	void SetPeerName(const FString& InPeerName);

	/** Returns the list of audio channels currently visible in the session. */
	UFUNCTION(BlueprintCallable, Category = "Link4UE|Audio")
	TArray<FLink4UEChannel> GetChannels() const;

	// --- Delegates ---

	UPROPERTY(BlueprintAssignable, Category = "Link4UE")
	FLink4UEOnNumPeersChanged OnNumPeersChanged;

	UPROPERTY(BlueprintAssignable, Category = "Link4UE")
	FLink4UEOnTempoChanged OnTempoChanged;

	UPROPERTY(BlueprintAssignable, Category = "Link4UE")
	FLink4UEOnStartStopChanged OnStartStopChanged;

	/** Fired each time a beat boundary is crossed. BeatNumber is the integer beat,
	 *  bIsPhaseZero is true when this beat is also a quantum boundary (bar start). */
	UPROPERTY(BlueprintAssignable, Category = "Link4UE|Beat")
	FLink4UEOnBeat OnBeat;

	/** Fired when a quantum boundary (phase == 0) is crossed — i.e. the start of a bar. */
	UPROPERTY(BlueprintAssignable, Category = "Link4UE|Beat")
	FLink4UEOnPhaseZero OnPhaseZero;

	/** Fired when the set of Link Audio channels changes (peer joins/leaves/renames). */
	UPROPERTY(BlueprintAssignable, Category = "Link4UE|Audio")
	FLink4UEOnChannelsChanged OnChannelsChanged;

private:
	/** Apply all settings to the Link instance. */
	void ApplySettings();

	/** Rebuild Submix→LinkAudio send routes (depends on AudioDevice). */
	void RebuildAudioSends();

	/** Rebuild LinkAudio→Submix receive routes (depends on AudioDevice + channels). */
	void RebuildAudioReceives();

	/** Sync channel names from live session (handles peer renames). */
	void SyncChannelNames();

	/** Per-frame tick driven by FTSTicker. */
	bool Tick(float DeltaTime);

	/** Cached snapshot, updated each tick. */
	FLink4UESessionSnapshot Snapshot;

	/** Quantum (local interpretation, not shared). */
	std::atomic<double> Quantum{4.0};

	/** Pending callback values from Link thread. */
	std::atomic<int32> PendingNumPeers{0};
	std::atomic<double> PendingTempo{120.0};
	std::atomic<bool> PendingIsPlaying{false};

	/** Dirty flags set by Link-thread callbacks, consumed on GameThread tick. */
	std::atomic<bool> bNumPeersDirty{false};
	std::atomic<bool> bTempoDirty{false};
	std::atomic<bool> bStartStopDirty{false};
	std::atomic<bool> bChannelsDirty{false};

	/** Previous beat floor — used for beat/phase-zero edge detection. */
	int32 PrevBeatFloor = -1;

	/** Opaque Link instance — forward declared to keep Link headers out of public API.
	 *  Raw pointer because UHT-generated code instantiates TUniquePtr destructor
	 *  in .gen.cpp where FLinkInstance is incomplete. */
	struct FLinkInstance;
	FLinkInstance* LinkInstance = nullptr;

	/** Ticker delegate handle. */
	FTSTicker::FDelegateHandle TickHandle;

	/** AudioDevice creation callback — rebuilds routes when device becomes available. */
	void OnAudioDeviceCreated(Audio::FDeviceId DeviceId);
	FDelegateHandle AudioDeviceCreatedHandle;

	/** True while send routes have not been successfully built yet. */
	bool bSendRoutesPending = false;

	/** True while receive routes have not been successfully built yet. */
	bool bReceiveRoutesPending = false;

	/** True while we are inside RebuildAudio* — suppresses re-entrant channel dirty flags. */
	bool bIsRebuilding = false;
};

// Copyright Tokyologist. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
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

// Delegates — fired on GameThread
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLink4UEOnNumPeersChanged, int32, NumPeers);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLink4UEOnTempoChanged, double, BPM);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLink4UEOnStartStopChanged, bool, bIsPlaying);

/**
 * Engine-lifetime subsystem that owns the Ableton Link instance.
 * One per process — safe across PIE sessions and editor use.
 */
UCLASS()
class LINK4UE_API ULink4UESubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	ULink4UESubsystem();
	virtual ~ULink4UESubsystem() override;

	// USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// --- Query (GameThread) ---

	/** Returns the most recent session snapshot (updated each tick). */
	UFUNCTION(BlueprintCallable, Category = "Link4UE")
	const FLink4UESessionSnapshot& GetSessionSnapshot() const { return Snapshot; }

	// --- Mutators (GameThread, thread-safe via Link API) ---

	UFUNCTION(BlueprintCallable, Category = "Link4UE")
	void SetTempo(double BPM);

	UFUNCTION(BlueprintCallable, Category = "Link4UE")
	void SetQuantum(double InQuantum);

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

	// --- Delegates ---

	UPROPERTY(BlueprintAssignable, Category = "Link4UE")
	FLink4UEOnNumPeersChanged OnNumPeersChanged;

	UPROPERTY(BlueprintAssignable, Category = "Link4UE")
	FLink4UEOnTempoChanged OnTempoChanged;

	UPROPERTY(BlueprintAssignable, Category = "Link4UE")
	FLink4UEOnStartStopChanged OnStartStopChanged;

private:
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

	/** Opaque Link instance — forward declared to keep Link headers out of public API.
	 *  Raw pointer because UHT-generated code instantiates TUniquePtr destructor
	 *  in .gen.cpp where FLinkInstance is incomplete. */
	struct FLinkInstance;
	FLinkInstance* LinkInstance = nullptr;

	/** Ticker delegate handle. */
	FTSTicker::FDelegateHandle TickHandle;
};

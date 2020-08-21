// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include <chrono>

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "LinkObject.generated.h"

// Delegates
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLinkOnNumPeersUpdate, int, NumPeers);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLinkOnTempoUpdate, float, BPM);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLinkOnStartStopUpdate, bool, bIsPlaying);

// Forward declaration
namespace ableton { class Link; }

/**
 * 
 */
UCLASS()
class LINKFORUNREAL_API ULinkObject : public UObject
{
	GENERATED_BODY()

public: // Public Member Functions
	// Deprecated constructor, ObjectInitializer is no longer needed but is supported for older classes.
	ULinkObject(const FObjectInitializer& ObjectInitializer);

	// MUST be called before in use.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	void Init(float BPM);

	// Called before destroying the object.
	virtual void BeginDestroy() override;

	// Returns true if ableton link is enabled.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	bool IsEnabled() const;

	// Enables ableton link.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	void Enable();

	// Disables ableton link.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	void Disable();

	// Returns true if StartStopSync is enabled.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	bool IsStartStopSyncEnabled() const;

	// Enables StartStopSync.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	void EnableStartStopSync();

	// Disables StartStopSync.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	void DisableStartStopSync();

	// Get number of peers currently connected to the same session.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	int NumPeers() const;

	// Start the session.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	void Start();

	// Stop the session.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	void Stop();

	// Returns true if the session is playing.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	bool IsPlaying() const;

	// Returns beat at the time.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	float BeatAtTime() const;

	// Returns phase at the time.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	float PhaseAtTime() const;

	// Returns the current tempo of the session.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	float GetTempo() const;

	// Set the session tempo.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	void SetTempo(float Tempo);

	// Returns the quantum.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	float GetQuantum() const;

	// Set the quantum.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	void SetQuantum(float NewQuantum);

	// Returns the elapsed time.
	UFUNCTION(BlueprintCallable, Category = "AbletonLink")
	FTimespan GetSessionTime() const;

private: // Private Member Functions

// DO NOT CALL THESE BY YOUR SELF. Should ONLY be called by AbletonLink.
	void NumPeersCallback(std::size_t NumPeers);
	void TempoCallback(double BPM);
	void StartStopCallback(bool bIsPlaying);

	// Returns the current timestamp.
	std::chrono::microseconds Now() const;

public: // Public Member Variables

	UPROPERTY(BlueprintAssignable, Category = "AbletonLink")
	FLinkOnNumPeersUpdate OnNumPeersUpdate;

	UPROPERTY(BlueprintAssignable, Category = "AbletonLink")
	FLinkOnTempoUpdate OnTempoUpdate;

	UPROPERTY(BlueprintAssignable, Category = "AbletonLink")
	FLinkOnStartStopUpdate OnStartStopUpdate;

private: // Public Member Variables
	ableton::Link* LinkPtr;

	double Quantum;

	bool bIsInitialized;
};

// Test Comment
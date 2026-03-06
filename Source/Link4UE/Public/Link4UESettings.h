// Copyright Tokyologist. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Link4UESettings.generated.h"

/** Quantum presets — matches Ableton Live's global quantization options. */
UENUM(BlueprintType)
enum class ELink4UEQuantum : uint8
{
	Bars_8		UMETA(DisplayName = "8 Bars"),
	Bars_4		UMETA(DisplayName = "4 Bars"),
	Bars_2		UMETA(DisplayName = "2 Bars"),
	Bar_1		UMETA(DisplayName = "1 Bar"),
	Half		UMETA(DisplayName = "1/2"),
	HalfT		UMETA(DisplayName = "1/2T"),
	Quarter		UMETA(DisplayName = "1/4"),
	QuarterT	UMETA(DisplayName = "1/4T"),
	Eighth		UMETA(DisplayName = "1/8"),
	EighthT		UMETA(DisplayName = "1/8T"),
	Sixteenth	UMETA(DisplayName = "1/16"),
	SixteenthT	UMETA(DisplayName = "1/16T"),
	ThirtySecond UMETA(DisplayName = "1/32"),
};

/** Convert a quantum preset to its beat value. */
LINK4UE_API double Link4UEQuantumToBeats(ELink4UEQuantum Preset);

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Link4UE"))
class LINK4UE_API ULink4UESettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
#endif

	// --- Connection ---

	/** Automatically join the Link network when the engine starts.
	 *  If false, call EnableLink() from Blueprint to connect manually. */
	UPROPERTY(Config, EditAnywhere, Category = "Connection", meta = (
		DisplayName = "Auto Connect"))
	bool bAutoConnect = true;

	/** Synchronize transport start/stop state with other Link peers. */
	UPROPERTY(Config, EditAnywhere, Category = "Connection", meta = (
		DisplayName = "Start/Stop Sync"))
	bool bStartStopSync = true;

	// --- LinkAudio ---

	/** Enable Link Audio channel streaming on startup. */
	UPROPERTY(Config, EditAnywhere, Category = "Connection", meta = (
		DisplayName = "Enable Link Audio"))
	bool bEnableLinkAudio = false;

	/** Display name advertised to other Link Audio peers. */
	UPROPERTY(Config, EditAnywhere, Category = "Connection", meta = (
		DisplayName = "Peer Name"))
	FString PeerName = TEXT("Unreal");

	// --- Defaults ---

	/** Initial tempo (BPM) used when creating the Link session.
	 *  Once peers connect, the tempo is shared across the session. */
	UPROPERTY(Config, EditAnywhere, Category = "Defaults", meta = (
		DisplayName = "Default Tempo (BPM)",
		ClampMin = "20.0", ClampMax = "999.0"))
	double DefaultTempo = 120.0;

	/** Phase synchronization unit.
	 *  Matches Ableton Live's global quantization options. */
	UPROPERTY(Config, EditAnywhere, Category = "Defaults", meta = (
		DisplayName = "Default Quantum"))
	ELink4UEQuantum DefaultQuantum = ELink4UEQuantum::Bar_1;
};

// Copyright YUKITAKA. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundSubmix.h"
#include "Link4UETypes.generated.h"

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

/** Defines a Submix → LinkAudio send route. */
USTRUCT(BlueprintType)
struct LINK4UE_API FLink4UEAudioSend
{
	GENERATED_BODY()

	/** Unique route identifier, auto-generated on creation. */
	UPROPERTY(Config, VisibleAnywhere, BlueprintReadOnly, Category = "Link4UE")
	FGuid RouteId;

	/** Source Submix to capture audio from. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Link4UE")
	TSoftObjectPtr<USoundSubmix> Submix;

	/** Channel name prefix advertised to peers.
	 *  Empty = use the Submix asset name.
	 *  For Submixes with 3+ channels, mono Sinks are named "{Prefix}_{index}". */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Link4UE")
	FString ChannelNamePrefix;
};

/** Defines a LinkAudio → Submix receive route. */
USTRUCT(BlueprintType)
struct LINK4UE_API FLink4UEAudioReceive
{
	GENERATED_BODY()

	/** Stable channel identifier (hex). Set by dropdown, not user-editable. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Link4UE")
	FString ChannelId;

	/** Channel display name (auto-updated on rename). */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Link4UE")
	FString ChannelName;

	/** Target Submix for audio output. Empty = Master Submix. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Link4UE")
	TSoftObjectPtr<USoundSubmix> Submix;
};

// Copyright YUKITAKA. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Link4UETypes.h"
#include "Engine/DeveloperSettings.h"
#include "Link4UESettings.generated.h"

DECLARE_MULTICAST_DELEGATE_OneParam(FLink4UEOnSettingsChanged, FName /*PropertyName*/);

UCLASS(Config = EditorPerProjectUserSettings, meta = (DisplayName = "Link4UE"))
class LINK4UE_API ULink4UESettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	static FLink4UEOnSettingsChanged OnSettingsChanged;
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

	// --- Audio Routing ---

	/** Submixes to send to the Link Audio network. */
	UPROPERTY(Config, EditAnywhere, Category = "Audio Routing", meta = (
		DisplayName = "Audio Sends"))
	TArray<FLink4UEAudioSend> AudioSends;

	/** Link Audio channels to receive and route to Submixes. */
	UPROPERTY(Config, EditAnywhere, Category = "Audio Routing", meta = (
		DisplayName = "Audio Receives"))
	TArray<FLink4UEAudioReceive> AudioReceives;
};

// Copyright Tokyologist. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Link4UESettings.generated.h"

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

	// --- Defaults ---

	/** Initial tempo (BPM) used when creating the Link session.
	 *  Once peers connect, the tempo is shared across the session. */
	UPROPERTY(Config, EditAnywhere, Category = "Defaults", meta = (
		DisplayName = "Default Tempo (BPM)",
		ClampMin = "20.0", ClampMax = "999.0"))
	double DefaultTempo = 120.0;

	/** Beats per bar used for phase calculation.
	 *  4.0 corresponds to 4/4 time. */
	UPROPERTY(Config, EditAnywhere, Category = "Defaults", meta = (
		DisplayName = "Default Quantum",
		ClampMin = "1.0"))
	double DefaultQuantum = 4.0;
};

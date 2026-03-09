// Temporary test class — verifying UCLASS(Config) on UEngineSubsystem.
// Delete after experiment is complete.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "ConfigSubsystemTest.generated.h"

UCLASS(Config = EditorPerProjectUserSettings)
class LINK4UE_API UConfigSubsystemTest : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// --- Test properties ---

	UPROPERTY(Config, EditAnywhere, Category = "Test")
	bool bTestBool = false;

	UPROPERTY(Config, EditAnywhere, Category = "Test")
	FString TestString = TEXT("Default");

	UPROPERTY(Config, EditAnywhere, Category = "Test")
	float TestFloat = 42.0f;

	UPROPERTY(Config, EditAnywhere, Category = "Test")
	TArray<FString> TestArray;

	// --- API ---

	UFUNCTION(BlueprintCallable, Category = "Test")
	void SetTestString(const FString& NewValue);

	UFUNCTION(BlueprintCallable, Category = "Test")
	void AddTestArrayEntry(const FString& Entry);

	UFUNCTION(BlueprintCallable, Category = "Test")
	void SaveSettings();

	UFUNCTION(BlueprintCallable, Category = "Test")
	void LogCurrentValues() const;
};

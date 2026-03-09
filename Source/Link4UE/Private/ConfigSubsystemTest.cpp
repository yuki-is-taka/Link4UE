// Temporary test class — verifying UCLASS(Config) on UEngineSubsystem.

#include "ConfigSubsystemTest.h"

#if WITH_EDITOR
#include "ISettingsModule.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogConfigTest, Log, All);

void UConfigSubsystemTest::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UE_LOG(LogConfigTest, Log,
		TEXT("ConfigSubsystemTest: Initialized — bTestBool=%s, TestString='%s', TestFloat=%.1f, TestArray.Num=%d"),
		bTestBool ? TEXT("true") : TEXT("false"),
		*TestString,
		TestFloat,
		TestArray.Num());

#if WITH_EDITOR
	ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
	if (SettingsModule)
	{
		SettingsModule->RegisterSettings(
			TEXT("Project"),
			TEXT("Plugins"),
			TEXT("ConfigSubsystemTest"),
			FText::FromString(TEXT("Config Subsystem Test")),
			FText::FromString(TEXT("Temporary test for UCLASS(Config) on UEngineSubsystem")),
			this
		);
		UE_LOG(LogConfigTest, Log, TEXT("ConfigSubsystemTest: Registered in Project Settings"));
	}
#endif
}

void UConfigSubsystemTest::Deinitialize()
{
#if WITH_EDITOR
	ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
	if (SettingsModule)
	{
		SettingsModule->UnregisterSettings(
			TEXT("Project"),
			TEXT("Plugins"),
			TEXT("ConfigSubsystemTest")
		);
	}
#endif

	UE_LOG(LogConfigTest, Log, TEXT("ConfigSubsystemTest: Deinitialized"));
	Super::Deinitialize();
}

void UConfigSubsystemTest::SetTestString(const FString& NewValue)
{
	TestString = NewValue;
	UE_LOG(LogConfigTest, Log, TEXT("ConfigSubsystemTest: TestString set to '%s'"), *NewValue);
}

void UConfigSubsystemTest::AddTestArrayEntry(const FString& Entry)
{
	TestArray.Add(Entry);
	UE_LOG(LogConfigTest, Log, TEXT("ConfigSubsystemTest: Added '%s' to TestArray (Num=%d)"),
		*Entry, TestArray.Num());
}

void UConfigSubsystemTest::SaveSettings()
{
	SaveConfig();
	UE_LOG(LogConfigTest, Log, TEXT("ConfigSubsystemTest: Config saved"));
}

void UConfigSubsystemTest::LogCurrentValues() const
{
	UE_LOG(LogConfigTest, Log,
		TEXT("ConfigSubsystemTest: bTestBool=%s, TestString='%s', TestFloat=%.1f"),
		bTestBool ? TEXT("true") : TEXT("false"),
		*TestString,
		TestFloat);

	for (int32 i = 0; i < TestArray.Num(); ++i)
	{
		UE_LOG(LogConfigTest, Log, TEXT("  TestArray[%d] = '%s'"), i, *TestArray[i]);
	}
}

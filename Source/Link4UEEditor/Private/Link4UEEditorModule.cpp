// Copyright YUKITAKA. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Link4UESubsystem.h"
#include "Link4UESettingsCustomization.h"

class FLink4UEEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FPropertyEditorModule& PropertyModule =
			FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

		PropertyModule.RegisterCustomClassLayout(
			ULink4UESubsystem::StaticClass()->GetFName(),
			FOnGetDetailCustomizationInstance::CreateStatic(
				&FLink4UESettingsCustomization::MakeInstance));

		PropertyModule.RegisterCustomPropertyTypeLayout(
			TEXT("Link4UEAudioReceive"),
			FOnGetPropertyTypeCustomizationInstance::CreateStatic(
				&FLink4UEAudioReceiveCustomization::MakeInstance));
	}

	virtual void ShutdownModule() override
	{
		if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
		{
			FPropertyEditorModule& PropertyModule =
				FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
			PropertyModule.UnregisterCustomClassLayout(
				ULink4UESubsystem::StaticClass()->GetFName());
			PropertyModule.UnregisterCustomPropertyTypeLayout(
				TEXT("Link4UEAudioReceive"));
		}
	}
};

IMPLEMENT_MODULE(FLink4UEEditorModule, Link4UEEditor)

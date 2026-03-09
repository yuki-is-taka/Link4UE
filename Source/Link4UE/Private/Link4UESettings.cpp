// Copyright YUKITAKA. All Rights Reserved.

#include "Link4UESettings.h"

#if WITH_EDITOR
FLink4UEOnSettingsChanged ULink4UESettings::OnSettingsChanged;

void ULink4UESettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	const FName PropName = PropertyChangedEvent.GetMemberPropertyName();
	OnSettingsChanged.Broadcast(PropName);
}

FText ULink4UESettings::GetSectionText() const
{
	return NSLOCTEXT("Link4UE", "SettingsSection", "Link4UE");
}

FText ULink4UESettings::GetSectionDescription() const
{
	return NSLOCTEXT("Link4UE", "SettingsDescription",
		"Ableton Link synchronization settings.");
}
#endif

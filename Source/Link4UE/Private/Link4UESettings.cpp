// Copyright Tokyologist. All Rights Reserved.

#include "Link4UESettings.h"

#if WITH_EDITOR
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

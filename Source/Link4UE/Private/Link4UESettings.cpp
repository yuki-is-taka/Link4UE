// Copyright YUKITAKA. All Rights Reserved.

#include "Link4UESettings.h"

double Link4UEQuantumToBeats(ELink4UEQuantum Preset)
{
	switch (Preset)
	{
	case ELink4UEQuantum::Bars_8:       return 32.0;
	case ELink4UEQuantum::Bars_4:       return 16.0;
	case ELink4UEQuantum::Bars_2:       return 8.0;
	case ELink4UEQuantum::Bar_1:        return 4.0;
	case ELink4UEQuantum::Half:         return 2.0;
	case ELink4UEQuantum::HalfT:        return 4.0 / 3.0;
	case ELink4UEQuantum::Quarter:      return 1.0;
	case ELink4UEQuantum::QuarterT:     return 2.0 / 3.0;
	case ELink4UEQuantum::Eighth:       return 0.5;
	case ELink4UEQuantum::EighthT:      return 1.0 / 3.0;
	case ELink4UEQuantum::Sixteenth:    return 0.25;
	case ELink4UEQuantum::SixteenthT:   return 1.0 / 6.0;
	case ELink4UEQuantum::ThirtySecond: return 0.125;
	default:                            return 4.0;
	}
}

#if WITH_EDITOR
FLink4UEOnSettingsChanged ULink4UESettings::OnSettingsChanged;

void ULink4UESettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	OnSettingsChanged.Broadcast();
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

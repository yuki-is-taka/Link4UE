// Copyright Tokyologist. All Rights Reserved.

#include "Link4UESettingsCustomization.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"

TSharedRef<IDetailCustomization> FLink4UESettingsCustomization::MakeInstance()
{
	return MakeShareable(new FLink4UESettingsCustomization);
}

void FLink4UESettingsCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	// Ensure category ordering: Connection first, then Defaults
	IDetailCategoryBuilder& ConnectionCategory = DetailBuilder.EditCategory(
		TEXT("Connection"), NSLOCTEXT("Link4UE", "ConnectionCategory", "Connection"));

	IDetailCategoryBuilder& DefaultsCategory = DetailBuilder.EditCategory(
		TEXT("Defaults"), NSLOCTEXT("Link4UE", "DefaultsCategory", "Defaults"));

	// Future: add status widgets, LinkAudio section, etc.
	// ConnectionCategory.AddCustomRow(...) for live status indicator
}

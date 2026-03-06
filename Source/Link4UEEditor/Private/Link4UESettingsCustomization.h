// Copyright Tokyologist. All Rights Reserved.

#pragma once

#include "IDetailCustomization.h"

/**
 * Detail customization for ULink4UESettings.
 * Provides a foundation for custom UI (status indicators, LinkAudio controls, etc.).
 */
class FLink4UESettingsCustomization : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};

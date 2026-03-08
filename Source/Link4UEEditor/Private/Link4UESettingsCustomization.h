// Copyright YUKITAKA. All Rights Reserved.

#pragma once

#include "IDetailCustomization.h"
#include "IPropertyTypeCustomization.h"

class FLink4UESettingsCustomization : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};

/**
 * Property type customization for FLink4UEAudioReceive.
 * Replaces the ChannelName/ChannelId fields with a dropdown of available LinkAudio channels.
 */
class FLink4UEAudioReceiveCustomization : public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle,
		FDetailWidgetRow& HeaderRow,
		IPropertyTypeCustomizationUtils& Utils) override;

	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle,
		IDetailChildrenBuilder& ChildBuilder,
		IPropertyTypeCustomizationUtils& Utils) override;

private:
	struct FChannelOption
	{
		FString Id;   // hex, empty for "(none)"
		FString Name; // display name
	};

	void RefreshChannelOptions();

	TSharedPtr<IPropertyHandle> ChannelIdHandle;
	TSharedPtr<IPropertyHandle> ChannelNameHandle;
	TArray<TSharedPtr<FChannelOption>> ChannelOptions;
	TSharedPtr<SComboBox<TSharedPtr<FChannelOption>>> ComboBoxWidget;
};

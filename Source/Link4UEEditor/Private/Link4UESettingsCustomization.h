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
 * Replaces the ChannelName text field with a dropdown of available LinkAudio channels.
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
	void RefreshChannelOptions();

	TSharedPtr<IPropertyHandle> ChannelNameHandle;
	TArray<TSharedPtr<FString>> ChannelOptions;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> ComboBoxWidget;
};

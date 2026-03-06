// Copyright Tokyologist. All Rights Reserved.

#include "Link4UESettingsCustomization.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "Link4UESubsystem.h"
#include "Widgets/Input/SComboBox.h"

// ---------------------------------------------------------------------------
// FLink4UESettingsCustomization
// ---------------------------------------------------------------------------

TSharedRef<IDetailCustomization> FLink4UESettingsCustomization::MakeInstance()
{
	return MakeShared<FLink4UESettingsCustomization>();
}

void FLink4UESettingsCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	DetailBuilder.EditCategory(TEXT("Connection"),
		NSLOCTEXT("Link4UE", "ConnectionCategory", "Connection"));
	DetailBuilder.EditCategory(TEXT("Defaults"),
		NSLOCTEXT("Link4UE", "DefaultsCategory", "Defaults"));
	DetailBuilder.EditCategory(TEXT("Audio Routing"),
		NSLOCTEXT("Link4UE", "AudioRoutingCategory", "Audio Routing"));
}

// ---------------------------------------------------------------------------
// FLink4UEAudioReceiveCustomization
// ---------------------------------------------------------------------------

TSharedRef<IPropertyTypeCustomization> FLink4UEAudioReceiveCustomization::MakeInstance()
{
	return MakeShared<FLink4UEAudioReceiveCustomization>();
}

void FLink4UEAudioReceiveCustomization::CustomizeHeader(
	TSharedRef<IPropertyHandle> PropertyHandle,
	FDetailWidgetRow& HeaderRow,
	IPropertyTypeCustomizationUtils& Utils)
{
	HeaderRow.NameContent()
	[
		PropertyHandle->CreatePropertyNameWidget()
	];
}

void FLink4UEAudioReceiveCustomization::CustomizeChildren(
	TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& Utils)
{
	ChannelNameHandle = PropertyHandle->GetChildHandle(TEXT("ChannelName"));
	TSharedPtr<IPropertyHandle> SubmixHandle = PropertyHandle->GetChildHandle(TEXT("Submix"));
	TSharedPtr<IPropertyHandle> ChannelIndexHandle = PropertyHandle->GetChildHandle(TEXT("SubmixChannelIndex"));

	RefreshChannelOptions();

	if (ChannelNameHandle.IsValid())
	{
		FString CurrentValue;
		ChannelNameHandle->GetValue(CurrentValue);

		TSharedPtr<FString> InitialSelection;
		for (const TSharedPtr<FString>& Option : ChannelOptions)
		{
			if (*Option == CurrentValue)
			{
				InitialSelection = Option;
				break;
			}
		}

		ChildBuilder.AddCustomRow(NSLOCTEXT("Link4UE", "ChannelName", "Channel Name"))
		.NameContent()
		[
			ChannelNameHandle->CreatePropertyNameWidget()
		]
		.ValueContent()
		.MinDesiredWidth(200.0f)
		[
			SAssignNew(ComboBoxWidget, SComboBox<TSharedPtr<FString>>)
			.OptionsSource(&ChannelOptions)
			.InitiallySelectedItem(InitialSelection)
			.OnComboBoxOpening_Lambda([this]()
			{
				// Refresh channel list each time the dropdown is opened
				RefreshChannelOptions();
				if (ComboBoxWidget.IsValid())
				{
					ComboBoxWidget->RefreshOptions();
				}
			})
			.OnSelectionChanged_Lambda(
				[this](TSharedPtr<FString> NewValue, ESelectInfo::Type)
				{
					if (NewValue.IsValid() && ChannelNameHandle.IsValid())
					{
						ChannelNameHandle->SetValue(*NewValue);
					}
				})
			.OnGenerateWidget_Lambda(
				[](TSharedPtr<FString> Item) -> TSharedRef<SWidget>
				{
					FText DisplayText = (Item.IsValid() && !Item->IsEmpty())
						? FText::FromString(*Item)
						: NSLOCTEXT("Link4UE", "EmptyChannel", "(none)");
					return SNew(STextBlock).Text(DisplayText);
				})
			.Content()
			[
				SNew(STextBlock)
				.Text_Lambda([this]() -> FText
				{
					if (ChannelNameHandle.IsValid())
					{
						FString Value;
						ChannelNameHandle->GetValue(Value);
						return FText::FromString(Value);
					}
					return FText::GetEmpty();
				})
			]
		];
	}

	if (SubmixHandle.IsValid())
	{
		ChildBuilder.AddProperty(SubmixHandle.ToSharedRef());
	}

	if (ChannelIndexHandle.IsValid())
	{
		ChildBuilder.AddProperty(ChannelIndexHandle.ToSharedRef());
	}
}

void FLink4UEAudioReceiveCustomization::RefreshChannelOptions()
{
	ChannelOptions.Empty();

	// Empty string option at top to allow clearing the selection
	ChannelOptions.Add(MakeShared<FString>(FString()));

	if (GEngine)
	{
		if (ULink4UESubsystem* Subsystem = GEngine->GetEngineSubsystem<ULink4UESubsystem>())
		{
			TArray<FLink4UEChannel> Channels = Subsystem->GetChannels();
			for (const FLink4UEChannel& Ch : Channels)
			{
				ChannelOptions.Add(MakeShared<FString>(Ch.Name));
			}
		}
	}
}

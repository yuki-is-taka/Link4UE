// Copyright YUKITAKA. All Rights Reserved.

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
	ChannelIdHandle = PropertyHandle->GetChildHandle(TEXT("ChannelId"));
	ChannelNameHandle = PropertyHandle->GetChildHandle(TEXT("ChannelName"));
	TSharedPtr<IPropertyHandle> SubmixHandle = PropertyHandle->GetChildHandle(TEXT("Submix"));

	RefreshChannelOptions();

	if (ChannelIdHandle.IsValid() && ChannelNameHandle.IsValid())
	{
		// Determine initial selection from stored ChannelId (or ChannelName fallback)
		FString CurrentId;
		ChannelIdHandle->GetValue(CurrentId);
		FString CurrentName;
		ChannelNameHandle->GetValue(CurrentName);

		TSharedPtr<FChannelOption> InitialSelection;
		for (const TSharedPtr<FChannelOption>& Option : ChannelOptions)
		{
			if (!CurrentId.IsEmpty() && Option->Id == CurrentId)
			{
				InitialSelection = Option;
				break;
			}
			if (CurrentId.IsEmpty() && !CurrentName.IsEmpty() && Option->Name == CurrentName)
			{
				InitialSelection = Option;
				break;
			}
		}

		ChildBuilder.AddCustomRow(NSLOCTEXT("Link4UE", "Channel", "Channel"))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(NSLOCTEXT("Link4UE", "ChannelLabel", "Channel"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		.ValueContent()
		.MinDesiredWidth(200.0f)
		[
			SAssignNew(ComboBoxWidget, SComboBox<TSharedPtr<FChannelOption>>)
			.OptionsSource(&ChannelOptions)
			.InitiallySelectedItem(InitialSelection)
			.OnComboBoxOpening_Lambda([this]()
			{
				RefreshChannelOptions();
				if (ComboBoxWidget.IsValid())
				{
					ComboBoxWidget->RefreshOptions();
				}
			})
			.OnSelectionChanged_Lambda(
				[this](TSharedPtr<FChannelOption> NewValue, ESelectInfo::Type)
				{
					if (NewValue.IsValid() && ChannelIdHandle.IsValid() && ChannelNameHandle.IsValid())
					{
						ChannelIdHandle->SetValue(NewValue->Id);
						ChannelNameHandle->SetValue(NewValue->Name);
					}
				})
			.OnGenerateWidget_Lambda(
				[](TSharedPtr<FChannelOption> Item) -> TSharedRef<SWidget>
				{
					FText DisplayText = (Item.IsValid() && !Item->Name.IsEmpty())
						? FText::FromString(Item->Name)
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
						if (!Value.IsEmpty())
						{
							return FText::FromString(Value);
						}
					}
					return NSLOCTEXT("Link4UE", "EmptyChannel", "(none)");
				})
			]
		];
	}

	if (SubmixHandle.IsValid())
	{
		ChildBuilder.AddProperty(SubmixHandle.ToSharedRef());
	}
}

void FLink4UEAudioReceiveCustomization::RefreshChannelOptions()
{
	ChannelOptions.Empty();

	// Empty option to allow clearing the selection
	ChannelOptions.Add(MakeShared<FChannelOption>());

	if (GEngine)
	{
		if (ULink4UESubsystem* Subsystem = GEngine->GetEngineSubsystem<ULink4UESubsystem>())
		{
			TArray<FLink4UEChannel> Channels = Subsystem->GetChannels();
			for (const FLink4UEChannel& Ch : Channels)
			{
				ChannelOptions.Add(MakeShared<FChannelOption>(FChannelOption{Ch.ChannelId, Ch.Name}));
			}
		}
	}
}

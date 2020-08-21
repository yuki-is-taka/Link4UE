// Fill out your copyright notice in the Description page of Project Settings.


#include "LinkObject.h"
#include "ableton/Link.hpp"

DEFINE_LOG_CATEGORY_STATIC(LogAbletonLink, Log, All);

// Deprecated constructor, ObjectInitializer is no longer needed but is supported for older classes.
ULinkObject::ULinkObject(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer), LinkPtr(nullptr), Quantum(4.), bIsInitialized(false)
{
	//
}

void ULinkObject::Init(float BPM)
{
	// Instanciate Ableton Link
	LinkPtr = new ableton::Link(static_cast<double>(BPM));

	// Setup callbacks
	auto NumPeersCallback = std::bind(&ULinkObject::NumPeersCallback, this, std::placeholders::_1);
	auto TempoCallback = std::bind(&ULinkObject::TempoCallback, this, std::placeholders::_1);
	auto StartStopCallback = std::bind(&ULinkObject::StartStopCallback, this, std::placeholders::_1);

	LinkPtr->setNumPeersCallback(NumPeersCallback);
	LinkPtr->setTempoCallback(TempoCallback);
	LinkPtr->setStartStopCallback(StartStopCallback);

	bIsInitialized = true;
}

void ULinkObject::BeginDestroy()
{
	Super::BeginDestroy();
	if (LinkPtr)
	{
		Disable();
		delete LinkPtr;
	}
}

bool ULinkObject::IsEnabled() const
{
	return LinkPtr && LinkPtr->isEnabled();
}

void ULinkObject::Enable()
{
	if (LinkPtr && !IsEnabled())
	{
		LinkPtr->enable(true);
		UE_LOG(LogAbletonLink, Log, TEXT("AbletonLink enabled"));
	}
}

void ULinkObject::Disable()
{
	if (LinkPtr && IsEnabled())
	{
		LinkPtr->enable(false);
		UE_LOG(LogAbletonLink, Log, TEXT("AbletonLink disabled"));
	}
}

bool ULinkObject::IsStartStopSyncEnabled() const
{
	return LinkPtr && LinkPtr->isStartStopSyncEnabled();
}

void ULinkObject::EnableStartStopSync()
{
	if (LinkPtr && !IsStartStopSyncEnabled())
	{
		LinkPtr->enableStartStopSync(true);
		UE_LOG(LogAbletonLink, Log, TEXT("StartStopSync enabled"));
	}
}

void ULinkObject::DisableStartStopSync()
{
	if (LinkPtr && IsStartStopSyncEnabled())
	{
		LinkPtr->enableStartStopSync(false);
		UE_LOG(LogAbletonLink, Log, TEXT("StartStopSync disabled"));
	}
}

int ULinkObject::NumPeers() const
{
	if (LinkPtr)
		return static_cast<int>(LinkPtr->numPeers());
	else
		return -1;
}

void ULinkObject::Start()
{
	if (LinkPtr)
	{
		auto SessionState = LinkPtr->captureAppSessionState();
		SessionState.setIsPlayingAndRequestBeatAtTime(true, Now(), 0., Quantum);
		LinkPtr->commitAppSessionState(SessionState);
		UE_LOG(LogAbletonLink, Log, TEXT("AbletonLink started"));
	}
	else
	{
		UE_LOG(LogAbletonLink, Log, TEXT("AbletonLink failed to start"));
	}
}

void ULinkObject::Stop()
{
	if (LinkPtr)
	{
		auto SessionState = LinkPtr->captureAppSessionState();
		SessionState.setIsPlaying(false, Now());
		LinkPtr->commitAppSessionState(SessionState);
		UE_LOG(LogAbletonLink, Log, TEXT("AbletonLink stopped"));
	}
	else
	{
		UE_LOG(LogAbletonLink, Log, TEXT("AbletonLink failed to stop"));
	}
}

bool ULinkObject::IsPlaying() const
{
	return LinkPtr && LinkPtr->captureAppSessionState().isPlaying();
}

float ULinkObject::BeatAtTime() const
{
	if(LinkPtr)
	{ 
		auto sessionState = LinkPtr->captureAppSessionState();
		return static_cast<float>(sessionState.beatAtTime(Now(), Quantum));
	}
	return -1.f;
}

float ULinkObject::PhaseAtTime() const
{
	if (LinkPtr)
	{
		auto SessionState = LinkPtr->captureAppSessionState();
		return static_cast<float>(SessionState.phaseAtTime(Now(), Quantum));
	}
	return -1.f;
}

float ULinkObject::GetTempo() const
{
	if (LinkPtr)
	{
		auto SessionState = LinkPtr->captureAppSessionState();
		return static_cast<float>(SessionState.tempo());
	}
	return -1.f;
}

void ULinkObject::SetTempo(float Tempo)
{
	if (LinkPtr)
	{
		auto SessionState = LinkPtr->captureAppSessionState();
		SessionState.setTempo(static_cast<double>(Tempo), Now());
		LinkPtr->commitAppSessionState(SessionState);
		UE_LOG(LogAbletonLink, Log, TEXT("New tempo is %.2f."), Tempo);
	}
	else
	{
		UE_LOG(LogAbletonLink, Log, TEXT("Failed to set tempo."));
	}
}

float ULinkObject::GetQuantum() const
{
	if (LinkPtr)
	{
		return static_cast<float>(Quantum);
	}
	return -1.f;
}

void ULinkObject::SetQuantum(float NewQuantum)
{
	this->Quantum = static_cast<double>(NewQuantum);
	UE_LOG(LogAbletonLink, Log, TEXT("New Quantum is %.2f."), Quantum);
}

FTimespan ULinkObject::GetSessionTime() const
{
	if (LinkPtr)
	{
		auto SessionState = LinkPtr->captureAppSessionState();
		return FTimespan::FromMicroseconds(static_cast<double>(SessionState.timeForIsPlaying().count()));
	}
	return FTimespan();
}

void ULinkObject::NumPeersCallback(std::size_t NumPeers)
{
	OnNumPeersUpdate.Broadcast(static_cast<int>(NumPeers));
}

void ULinkObject::TempoCallback(double BPM)
{
	OnTempoUpdate.Broadcast(static_cast<float>(BPM));
}

void ULinkObject::StartStopCallback(bool bIsPlaying)
{
	OnStartStopUpdate.Broadcast(bIsPlaying);
}

std::chrono::microseconds ULinkObject::Now() const
{
	if (LinkPtr)
	{
		return LinkPtr->clock().micros();
	}
	return std::chrono::microseconds();
}

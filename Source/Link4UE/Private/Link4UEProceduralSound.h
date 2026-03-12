// Copyright YUKITAKA. All Rights Reserved.

#pragma once

#include "Sound/SoundWaveProcedural.h"
#include "Sound/SoundGenerator.h"
#include "Link4UEProceduralSound.generated.h"

class FLink4UESoundGenerator;

// ---------------------------------------------------------------------------
// ULink4UEProceduralSound — minimal USoundWaveProcedural subclass that
//   provides an ISoundGenerator, bypassing the QueueAudio/GeneratePCMData path.
// ---------------------------------------------------------------------------

UCLASS()
class ULink4UEProceduralSound : public USoundWaveProcedural
{
	GENERATED_BODY()

public:
	virtual ISoundGeneratorPtr CreateSoundGenerator(
		const FSoundGeneratorInitParams& InParams) override;

	/** Raw pointer — safe because Generator lifetime is tied to this UObject.
	 *  Avoids atomic refcount churn on the audio callback hot path. */
	FLink4UESoundGenerator* GetGenerator() const { return Generator.Get(); }

private:
	TSharedPtr<FLink4UESoundGenerator> Generator;
};

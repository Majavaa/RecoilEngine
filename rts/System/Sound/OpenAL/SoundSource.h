/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef SOUNDSOURCE_H
#define SOUNDSOURCE_H

#include <string>
#include <memory>

#include <al.h>

#include "System/Misc/SpringTime.h"
#include "System/Sound/IAudioChannel.h"
#include "System/Sound/ISoundAttenuationModel.h"
#include "System/float3.h"

class SoundItem;
class MusicStream;

/**
 * @brief One soundsource which can play some sounds
 *
 * Construct some of them, and they can play SoundItems positioned anywhere in 3D-space for you.
 */
class CSoundSource
{
public:
	/// is ready after this
	CSoundSource();
	CSoundSource(CSoundSource&& src);
	CSoundSource(const CSoundSource& src) = delete;
	~CSoundSource();

	CSoundSource& operator = (CSoundSource&& src);
	CSoundSource& operator = (const CSoundSource& src) = delete;

    static constexpr float ROLLOFF_FACTOR = 5.0f;
    static constexpr float REFERENCE_DIST = 200.0f;

	void Update();
	void Delete();

    void ApplyAttenuationModel(const bool smooth);

	void UpdateVolume();
	bool IsValid() const { return (id != 0); };

	int GetCurrentPriority() const;
	bool IsPlaying(const bool checkOpenAl = false) const;
	void Stop();

	/// will stop a currently playing sound, if any
	void Play(IAudioChannel* channel, SoundItem* item, float3 pos, float3 velocity, float volume, bool relative = false);
	void PlayAsync(IAudioChannel* channel, size_t id, float3 pos, float3 velocity, float volume, float priority, bool relative = false);
	void PlayStream(IAudioChannel* channel, const std::string& file, float volume);
	void StreamStop();
	void StreamPause();
	float GetStreamTime();
	float GetStreamPlayTime();
    void ComputeCameraSpaceData();

	static void SetPitch(const float& newPitch) { globalPitch = newPitch; }
	static void SetHeightRolloffModifer(const float& mod) { heightRolloffModifier = mod; }

    bool IsIn3D() const { return in3D; }
    float3 GetPosition() const { return currentPosition; }
    ALuint GetId() const { return id; }

    static constexpr float VIEWPORT_VOLUME_REDUCTION_SPEED = 0.02f;

private:
	void swap(CSoundSource& other);

    void Initialize(IAudioChannel* channel, SoundItem* item, float3 pos, float3 velocity, float volume, bool relative);
    void EnableSpatialization();
    void DisableSpatialization();

    float GetSummedVolume() {
        return (currentChannel ? currentChannel->volume : 1.0f) * currentSoundItem.volume * currentVolume;
    }

	struct AsyncSoundItemData {
		IAudioChannel* channel = nullptr;

		size_t id = 0;

		float3 position;
		float3 velocity;

		float volume = 1.0f;
		float priority = 0.0f;

		bool relative = false;
	};

	// light-weight SoundItem with only the data needed for playback
	struct SoundItemData {
		size_t id = 0;

		unsigned int loopTime = 0;
		int priority = 0;

		float volume = 0.0f;
		float rolloff = 0.0f;
	};

private:
	// used to adjust the pitch to the GameSpeed (optional)
	static float globalPitch;

	// reduce the rolloff when the camera is height above the ground (so we still hear something in tab mode or far zoom)
	static float heightRolloffModifier;

private:
	ALuint id = 0;

    ALuint attenuationFilter = 0;
    float outerDistance = 0;
    float innerDistance = 0;
    float forwardDistance = 0;
    float2 viewportHalfExtents = float2();
    float terrainDistance;

    SoundAttenuationOutput attenuationOutput;

    float bufferId = 0;

    float cameraZoom;

    float3 currentPosition = float3(0.0f, 0.0f, 0.0f);

	SoundItemData currentSoundItem;
	AsyncSoundItemData asyncPlayItem;

	IAudioChannel* currentChannel = nullptr;
	std::unique_ptr <MusicStream> curStream;

	float currentVolume = 1.0f;
    float curViewportVolumeMultiplier = 0;

	spring_time loopStop {1e9};
    spring_time lastUpdate = spring_gettime();
	bool in3D = false;
	bool efxEnabled = false;
	int efxUpdates = 0;

    std::string name;

	ALfloat curHeightRolloffModifier = 1.0f;
};

#endif


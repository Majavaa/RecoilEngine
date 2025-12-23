/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "SoundSource.h"

#include <al.h>
#include <algorithm>
#include <climits>
#include <alc.h>
#include <cmath>
#include <efx.h>
#include <tuple>

#include "ALShared.h"
#include "Game/Camera.h"
#include "EFX.h"
#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/TraceRay.h"
#include "Rendering/GlobalRendering.h"
#include "System/Config/ConfigHandler.h"
#include "System/Log/ILog.h"
#include "System/Misc/SpringTime.h"
#include "System/Sound/OpenAL/EFXfuncs.h"
#include "System/Sound/SoundLog.h"
#include "System/Sound/IAudioChannel.h"
#include "MusicStream.h"
#include "SoundBuffer.h"
#include "SoundItem.h"

#include <AL/al.h>
#include <AL/efx.h>


#include "Sound.h" //remove when unified ElmoInMeters

#include "Sim/Misc/GlobalConstants.h"
#include "System/float3.h"

// used to adjust the pitch to the GameSpeed (optional)
float CSoundSource::globalPitch = 1.0f;

// reduce the rolloff when the camera is height above the ground (so we still hear something in tab mode or far zoom)
float CSoundSource::heightRolloffModifier = 1.0f;

void CSoundSource::swap(CSoundSource& r)
{
    //TODO makesure all data here is swapped
	std::swap(id, r.id);
	std::swap(currentChannel, r.currentChannel);
	std::swap(curStream, r.curStream);
	std::swap(currentVolume, r.currentVolume);
	std::swap(loopStop, r.loopStop);
	std::swap(in3D, r.in3D);
	std::swap(efxEnabled, r.efxEnabled);
	std::swap(efxUpdates, r.efxUpdates);
	std::swap(curHeightRolloffModifier, r.curHeightRolloffModifier);

	std::swap(currentSoundItem, r.currentSoundItem);
	std::swap(asyncPlayItem, r.asyncPlayItem);
}

CSoundSource::CSoundSource()
{
	alGenSources(1, &id);

	if (!CheckError("CSoundSource::CSoundSource")) {
		id = 0;
	} else {
		// alSourcef(id, AL_REFERENCE_DISTANCE, REFERENCE_DIST * ELMOS_TO_METERS);
		CheckError("CSoundSource::CSoundSource");
	}
}

CSoundSource::CSoundSource(CSoundSource&& src)
{
	// can't use naive/default move because `id` member has to be unique
	this->swap(src);
}

CSoundSource& CSoundSource::operator = (CSoundSource&& src)
{
	this->swap(src);
	return *this;
}

CSoundSource::~CSoundSource()
{
	Delete();
}

float SmoothTowards(float current, float target, float speed, float dt)
{
    const float t = 1.0f - std::exp(-speed * dt);
    return current + (target - current) * t;
}

float Curve(float t, float min, float max, float k)
{
    return min + (max - min) * std::pow(t, k);
}

void CSoundSource::ApplyAttenuationModel(bool smooth)
{
    if (!in3D)
        return;

    if (sound == nullptr) {
        LOG_L(L_ERROR, "Could not get sound singleton");
        return;
    }

    if (sound->GetAttenuationModel() == nullptr) {
        LOG_L(L_ERROR, "Could not get attenuation model");
        return;
    }

    attenuationOutput = sound->GetAttenuationModel()->Evaluate({ currentPosition });

    float totalValue = attenuationOutput.totalFactor;

    if (smooth)
        curViewportVolumeMultiplier = SmoothTowards(
            curViewportVolumeMultiplier,
            totalValue,
            VIEWPORT_VOLUME_REDUCTION_SPEED,
            globalRendering->lastFrameTime);
    else
        curViewportVolumeMultiplier = totalValue;

    float vol = currentVolume;

    vol = Curve(curViewportVolumeMultiplier, 0.0f, 1.0f, 3);

    if (name.contains("sizzle"))
        LOG_L(L_NOTICE, "val: %f, vol: %f", totalValue, vol);

    // alSourcef(id, AL_GAIN, vol);

    float filter = 1;

    if (curViewportVolumeMultiplier <= 1.0) {
        float factor = std::min(curViewportVolumeMultiplier / 1.0f, 1.0f);
        filter = Curve(factor, 0.1f, 1.0f, 0.75f);
    }

    // alFilterf(attenuationFilter, AL_LOWPASS_GAIN, 1);
    // alFilterf(attenuationFilter, AL_LOWPASS_GAINHF, filter);
    //
    // alSourcei(id, AL_DIRECT_FILTER, attenuationFilter);
}

void CSoundSource::Update()
{
	if (asyncPlayItem.id != 0) {
		// Sound::Update() holds mutex, soundItems can not be accessed concurrently
		Play(asyncPlayItem.channel, sound->GetSoundItem(asyncPlayItem.id), asyncPlayItem.position, asyncPlayItem.velocity, asyncPlayItem.volume, asyncPlayItem.relative);
		asyncPlayItem = AsyncSoundItemData();
	}

	if (currentSoundItem.id != 0) {
        if (configHandler->GetBool("snd_useAttenuationModel")) {
            ApplyAttenuationModel(true);
        } else {
            if (in3D && (efxEnabled != efx.Enabled())) {
                alSourcef(id, AL_AIR_ABSORPTION_FACTOR, (efx.Enabled()) ? efx.GetAirAbsorptionFactor() : 0);
                alSource3i(id, AL_AUXILIARY_SEND_FILTER, (efx.Enabled()) ? efx.sfxSlot : AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
                alSourcei(id, AL_DIRECT_FILTER, (efx.Enabled()) ? efx.sfxFilter : AL_FILTER_NULL);
                efxEnabled = efx.Enabled();
                efxUpdates = efx.updates;
            }

            if (heightRolloffModifier != curHeightRolloffModifier) {
                curHeightRolloffModifier = heightRolloffModifier;
                alSourcef(id, AL_ROLLOFF_FACTOR, ROLLOFF_FACTOR * currentSoundItem.rolloff * heightRolloffModifier);
            }
        }

		if (!IsPlaying(true) || ((currentSoundItem.loopTime > 0) && (spring_gettime() > loopStop)))
			Stop();
	}

	if (curStream) {
		if (curStream->IsFinished()) {
			Stop();
		} else {
			curStream->Update();
			CheckError("CSoundSource::Update");
		}
	}

	if (!configHandler->GetBool("snd_useAttenuationModel") && efxEnabled && (efxUpdates != efx.updates)) {
		// airAbsorption & LowPass aren't auto updated by OpenAL on change, so we need to do it per source
		alSourcef(id, AL_AIR_ABSORPTION_FACTOR, efx.GetAirAbsorptionFactor());
		alSourcei(id, AL_DIRECT_FILTER, efx.sfxFilter);
		efxUpdates = efx.updates;
	}
}

void CSoundSource::Delete()
{
	if (efxEnabled) {
		alSource3i(id, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
		alSourcei(id, AL_DIRECT_FILTER, AL_FILTER_NULL);
	}

	Stop();
	alDeleteSources(1, &id);
	CheckError("CSoundSource::Delete");
}

int CSoundSource::GetCurrentPriority() const
{
	if (asyncPlayItem.id != 0)
		return asyncPlayItem.priority;

	if (curStream)
		return INT_MAX;

	if (currentSoundItem.id == 0)
		return INT_MIN;

	return (currentSoundItem.priority);
}

bool CSoundSource::IsPlaying(const bool checkOpenAl) const
{
	if (curStream)
		return true;

	if (asyncPlayItem.id != 0)
		return true;

	if (currentSoundItem.id == 0)
		return false;

	// calling OpenAL has a high chance of generating a L2 cache miss, avoid if possible
	if (!checkOpenAl)
		return true;

	CheckError("CSoundSource::IsPlaying");
	ALint state;
	alGetSourcei(id, AL_SOURCE_STATE, &state);
	CheckError("CSoundSource::IsPlaying");
	return (state == AL_PLAYING);
}

void CSoundSource::Stop()
{
	alSourceStop(id);

	{
		SoundItem* item = nullptr;

		// callers marked * are mutex-guarded
		//   ::Delete via ~CSoundSource via CSound::Kill
		//   ::Play via ::Update (*)
		//   ::PlayStream via AudioChannel::StreamPlay (*)
		//   ::StreamStop via AudioChannel::StreamStop (*)
		//   AudioChannel::FindSourceAndPlay (*)
		if (sound != nullptr)
			item = sound->GetSoundItem(currentSoundItem.id);
		if (item != nullptr)
			item->StopPlay();

		currentSoundItem = {};
	}

	curStream.reset();

	if (currentChannel != nullptr) {
		IAudioChannel* oldChannel = currentChannel;
		currentChannel = nullptr;
		oldChannel->SoundSourceFinished(this);
	}
	CheckError("CSoundSource::Stop");
}

void CSoundSource::Initialize(IAudioChannel* channel, SoundItem* item, float3 pos, float3 velocity, float volume, bool relative)
{
    Stop();

    currentVolume = volume;
    currentSoundItem = { item->soundItemID, item->loopTime, item->priority, item->GetGain(), item->rolloff };
    currentChannel = channel;
    in3D = !relative && item->in3D;
    bufferId = item->GetSoundBufferID();
    currentPosition = pos;

    const SoundBuffer& itemBuffer = SoundBuffer::GetById(bufferId);
    alSourcei(id, AL_BUFFER, itemBuffer.GetId());

    float vol = GetSummedVolume();

    LOG_L(L_NOTICE, "name: %s, vol: %f", item->name.c_str(), vol);

    alSourcef(id, AL_GAIN, vol);
    alSourcef(id, AL_PITCH, item->GetPitch() * globalPitch);

    velocity *= item->dopplerScale * ELMOS_TO_METERS;
    alSource3f(id, AL_VELOCITY, velocity.x, velocity.y, velocity.z);
    alSourcei(id, AL_LOOPING, (item->loopTime > 0) ? AL_TRUE : AL_FALSE);

    loopStop = spring_gettime() + spring_msecs(item->loopTime);

	if (in3D) {
        EnableSpatialization();
    } else {
        DisableSpatialization();
    }
}

void CSoundSource::EnableSpatialization()
{
    const SoundBuffer& itemBuffer = SoundBuffer::GetById(bufferId);

    if (itemBuffer.GetChannels() > 1)
        LOG_L(L_WARNING, "Can not play non-mono \"%s\" in 3d.", itemBuffer.GetFilename().c_str());

    // alSourcei(id, AL_SOURCE_RELATIVE, AL_FALSE);
    // float3 pos = currentPosition * ELMOS_TO_METERS;
    // alSource3f(id, AL_POSITION, pos.x, pos.y, pos.z);

    if (configHandler->GetBool("snd_useAttenuationModel")) {

        if (attenuationFilter == 0) {
            alGenFilters(1, &attenuationFilter);
            alFilteri(attenuationFilter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
        }

        alFilterf(attenuationFilter, AL_LOWPASS_GAIN, 1);
        alFilterf(attenuationFilter, AL_LOWPASS_GAINHF, 1);

        ApplyAttenuationModel(false);

    } else {
        if (efx.Enabled()) {
            efxEnabled = true;
            alSourcef(id, AL_AIR_ABSORPTION_FACTOR, efx.GetAirAbsorptionFactor());
            alSource3i(id, AL_AUXILIARY_SEND_FILTER, efx.sfxSlot, 0, AL_FILTER_NULL);
            alSourcei(id, AL_DIRECT_FILTER, efx.sfxFilter);
            efxUpdates = efx.updates;
        }

        alSourcei(id, AL_SOURCE_RELATIVE, AL_FALSE);
        float3 pos = currentPosition * ELMOS_TO_METERS;
        alSource3f(id, AL_POSITION, pos.x, pos.y, pos.z);

        curHeightRolloffModifier = heightRolloffModifier;
        alSourcef(id, AL_ROLLOFF_FACTOR, ROLLOFF_FACTOR * currentSoundItem.rolloff * heightRolloffModifier);
    }

#if defined(__APPLE__) || defined(__OpenBSD__)
    alSourcef(id, AL_MAX_DISTANCE, 1000000.0f);
    // Max distance is too small by default on my Mac...
    ALfloat gain = GetSummedVolume();
    if (gain > 1.0f) {
        // OpenAL on Mac cannot handle AL_GAIN > 1 well, so we will adjust settings to get the same output with AL_GAIN = 1.
        const ALint model = alGetInteger(AL_DISTANCE_MODEL);
        const ALfloat rolloff = ROLLOFF_FACTOR * currentSoundItem.rolloff * heightRolloffModifier;
        const ALfloat refDist = REFERENCE_DIST * ELMOS_TO_METERS;

        if ((model == AL_INVERSE_DISTANCE_CLAMPED) || (model == AL_INVERSE_DISTANCE)) {
            alSourcef(id, AL_REFERENCE_DISTANCE, ((gain - 1.0f) * refDist / rolloff) + refDist);
            alSourcef(id, AL_ROLLOFF_FACTOR, (gain + rolloff - 1.0f) / gain);
            alSourcef(id, AL_GAIN, 1.0f);
        }
    } else {
        alSourcef(id, AL_REFERENCE_DISTANCE, REFERENCE_DIST * ELMOS_TO_METERS);
    }
#endif
}

void CSoundSource::DisableSpatialization()
{
    alSourcei(id, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcef(id, AL_ROLLOFF_FACTOR, 0.f);
    alSource3f(id, AL_POSITION, 0.0f, 0.0f, -1.0f * ELMOS_TO_METERS);

#if defined(__APPLE__) || defined(__OpenBSD__)
    alSourcef(id, AL_REFERENCE_DISTANCE, REFERENCE_DIST * ELMOS_TO_METERS);
#endif

    if (!efxEnabled)
        return;

    alSource3i(id, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
    alSourcei(id, AL_DIRECT_FILTER, AL_FILTER_NULL);
    efxEnabled = false;
}

void CSoundSource::Play(IAudioChannel* channel, SoundItem* item, float3 pos, float3 velocity, float volume, bool relative)
{
    assert(!curStream);
    assert(channel);

    if (!item->PlayNow())
        return;

    Initialize(channel, item, pos, velocity, volume, relative);

	alSourcePlay(id);

    const SoundBuffer& itemBuffer = SoundBuffer::GetById(bufferId);
    if (bufferId == 0)
        LOG_L(L_WARNING, "CSoundSource::Play: Empty buffer for item %s (file %s)", item->name.c_str(), itemBuffer.GetFilename().c_str());

    CheckError("CSoundSource::Play");
}

void CSoundSource::PlayAsync(IAudioChannel* channel, size_t id, float3 pos, float3 velocity, float volume, float priority, bool relative)
{
	asyncPlayItem.channel  = channel;
	asyncPlayItem.id       = id;

	asyncPlayItem.position = pos;
	asyncPlayItem.velocity = velocity;

	asyncPlayItem.volume   = volume;
	asyncPlayItem.priority = priority;

	asyncPlayItem.relative = relative;
}


void CSoundSource::PlayStream(IAudioChannel* channel, const std::string& file, float volume)
{
	// stop any current playback
	Stop();

	if (!curStream)
		curStream = std::make_unique <MusicStream> ();

	// OpenAL params
	currentChannel = channel;
	currentVolume = volume;
	in3D = false;

	if (efxEnabled) {
		alSource3i(id, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
		alSourcei(id, AL_DIRECT_FILTER, AL_FILTER_NULL);
		efxEnabled = false;
	}

	alSource3f(id, AL_POSITION,       0.0f, 0.0f, 0.0f);
	alSourcef(id, AL_GAIN,            volume);
	alSourcef(id, AL_PITCH,           globalPitch);
	alSource3f(id, AL_VELOCITY,       0.0f,  0.0f,  0.0f);
	alSource3f(id, AL_DIRECTION,      0.0f,  0.0f,  0.0f);
	alSourcef(id, AL_ROLLOFF_FACTOR,  0.0f);
	alSourcei(id, AL_SOURCE_RELATIVE, AL_TRUE);

	// COggStreams only appends buffers, giving errors when a buffer of another format is still assigned
	alSourcei(id, AL_BUFFER, AL_NONE);
	curStream->Play(file, volume, id);
	curStream->Update();
	CheckError("CSoundSource::Update");
}

void CSoundSource::StreamStop()
{
	if (!curStream)
		return;

	Stop();
}

void CSoundSource::StreamPause()
{
	if (!curStream)
		return;

	if (curStream->TogglePause())
		alSourcePause(id);
	else
		alSourcePlay(id);
}

float CSoundSource::GetStreamTime()
{
	return curStream
		? curStream->GetTotalTime()
		: 0.0f
	;
}

float CSoundSource::GetStreamPlayTime()
{
	return curStream
		? curStream->GetPlayTime()
		: 0.0f
	;
}

void CSoundSource::UpdateVolume()
{
	if (currentChannel == nullptr)
		return;

	if (curStream) {
		alSourcef(id, AL_GAIN, currentVolume * currentChannel->volume);
		return;
	}
	if (currentSoundItem.id != 0) {
		alSourcef(id, AL_GAIN, currentVolume * currentSoundItem.volume * currentChannel->volume);
		return;
	}
}


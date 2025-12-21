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
	std::swap(id, r.id);
	std::swap(curChannel, r.curChannel);
	std::swap(curStream, r.curStream);
	std::swap(curVolume, r.curVolume);
	std::swap(loopStop, r.loopStop);
	std::swap(in3D, r.in3D);
	std::swap(efxEnabled, r.efxEnabled);
	std::swap(efxUpdates, r.efxUpdates);
	std::swap(curHeightRolloffModifier, r.curHeightRolloffModifier);

	std::swap(curPlayingItem, r.curPlayingItem);
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

/// Distance in front of the camera at which volume attenuation reaches its maximum
static constexpr float FORWARD_ATTENUATION_RANGE = 8000.0f;

/// Distance behind the camera at which volume attenuation reaches its maximum
static constexpr float BACKWARD_ATTENUATION_RANGE = 300.0f;

/// Distance outside the viewport at which off-screen attenuation reaches its maximum
static constexpr float OUTER_ATTENUATION_RANGE = 1000.0f;

/// Percentage (0–1) of the viewport half-extents that is exempt from off-center attenuation
static constexpr float OFFCENTER_SAFE_ZONE_RATIO = 0.3f;

/// Maximum volume reduction applied when a sound is fully off-center
static constexpr float OFFCENTER_ATTENUATION_STRENGTH = 0.3f;

/// Scales how strongly camera zoom influences off-center attenuation
/// (zoomed out = stronger effect, zoomed in = weaker effect)
static constexpr float ZOOM_ATTENUATION_INFLUENCE = 0.2f;

CSoundSource::CSoundSource(CSoundSource&& src)
{
	// can't use naive/default move because `id` member has to be unique
	this->swap(src);
}

CSoundSource& CSoundSource::operator = (CSoundSource&& src) {
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

float Curve(float t, float min, float max, float k) {
    return min + (max - min) * std::pow(t, k);
}

void CSoundSource::ComputeCameraSpaceData() {
    if (!in3D) return;

    CCamera* playerCamera = CCameraHandler::GetCamera(CCamera::CAMTYPE_PLAYER);

    float3 toSound = currentPosition - playerCamera->GetPos();

    float camRight = playerCamera->GetRight().dot(toSound);
    float camUp = playerCamera->GetUp().dot(toSound);
    float camForward = playerCamera->GetForward().dot(toSound);

    innerDistance = sqrt(camRight * camRight + camUp * camUp);
    forwardDistance = camForward;

    float hfov = playerCamera->GetHFOV() * math::DEG_TO_RAD;
    float vfov = playerCamera->GetVFOV() * math::DEG_TO_RAD;

    float distance = std::abs(camForward);
    if (distance <= 0.0f) distance = 1.0f;

    float frustumWidth = distance * std::tan(hfov * 0.5f);
    float frustumHeight = distance * std::tan(vfov * 0.5f);

    const CUnit* hitUnit = nullptr;
    const CFeature* hitFeature = nullptr;

    terrainDistance = TraceRay::GuiTraceRay(
        playerCamera->GetPos(),
        playerCamera->GetForward(),
        FORWARD_ATTENUATION_RANGE,
        nullptr,
        hitUnit,
        hitFeature,
        true,
        true
    );

    viewportHalfExtents = float2(frustumWidth, frustumHeight);

    float outsideRight = std::max(0.0f, std::abs(camRight) - frustumWidth);
    float outsideUp = std::max(0.0f, std::abs(camUp) - frustumHeight);

    outerDistance = std::sqrt(outsideRight * outsideRight + outsideUp * outsideUp);
}

void CSoundSource::ApplyGainBasedOnVisiblity(bool smooth) {
    if (!in3D) return;

    float forwardValue = forwardDistance >= 0 ?
        forwardValue = std::clamp(1 - forwardDistance / FORWARD_ATTENUATION_RANGE, 0.0f, 1.0f):
        forwardValue = std::clamp(1 - (-forwardDistance) / BACKWARD_ATTENUATION_RANGE, 0.0f, 1.0f);

    float outerValue = std::clamp(1 - outerDistance / OUTER_ATTENUATION_RANGE, 0.0f, 1.0f);

    float innerMaxRadius = std::min(viewportHalfExtents.x, viewportHalfExtents.y);
    float innerMinRadius = innerMaxRadius * OFFCENTER_SAFE_ZONE_RATIO;

    float t = std::clamp((innerDistance - innerMinRadius) / (innerMaxRadius - innerMinRadius), 0.0f, 1.0f);

    float zoomFactor = std::clamp(terrainDistance == -1 ? 1.0f : terrainDistance / FORWARD_ATTENUATION_RANGE, 0.0f, 1.0f);

    float innerValue = 1.0f - t * (OFFCENTER_ATTENUATION_STRENGTH * zoomFactor);

    float totalValue = forwardValue * outerValue * innerValue;

    if (smooth)
        curViewportVolumeMultiplier = SmoothTowards(
            curViewportVolumeMultiplier,
            totalValue,
            VIEWPORT_VOLUME_REDUCTION_SPEED,
            globalRendering->lastFrameTime);
    else
        curViewportVolumeMultiplier = totalValue;

    float vol = curVolume;

    vol = Curve(curViewportVolumeMultiplier, 0.0f, 1.0f, 3);

    alSourcef(id, AL_GAIN, vol);

    float filter = 1;

    if (curViewportVolumeMultiplier <= 1.0) {
        float factor = std::min(curViewportVolumeMultiplier / 1.0f, 1.0f);
        filter = Curve(factor, 0.1f, 1.0f, 0.75f);
    }

    alFilterf(attenuationFilter, AL_LOWPASS_GAIN, 1);
    alFilterf(attenuationFilter, AL_LOWPASS_GAINHF, filter);
}

void CSoundSource::Update()
{
    ComputeCameraSpaceData();
    ApplyGainBasedOnVisiblity(true);

	if (asyncPlayItem.id != 0) {
		// Sound::Update() holds mutex, soundItems can not be accessed concurrently
		Play(asyncPlayItem.channel, sound->GetSoundItem(asyncPlayItem.id), asyncPlayItem.position, asyncPlayItem.velocity, asyncPlayItem.volume, asyncPlayItem.relative);
		asyncPlayItem = AsyncSoundItemData();
	}

    // LOG_L(L_WARNING, "Can not play non-mono \"%s\" in 3d.", itemBuffer.GetFilename().c_str());

    if (curPlayingItem.id != 0) {
        // if (in3D && (efxEnabled != efx.Enabled())) {
        //     alSourcef(id, AL_AIR_ABSORPTION_FACTOR, (efx.Enabled()) ? efx.GetAirAbsorptionFactor() : 0);
        //     alSource3i(id, AL_AUXILIARY_SEND_FILTER, (efx.Enabled()) ? efx.sfxSlot : AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
        //     alSourcei(id, AL_DIRECT_FILTER, (efx.Enabled()) ? efx.sfxFilter : AL_FILTER_NULL);
        //     efxEnabled = efx.Enabled();
        //     efxUpdates = efx.updates;
        // }

		if (heightRolloffModifier != curHeightRolloffModifier) {
			curHeightRolloffModifier = heightRolloffModifier;
            // LOG_L(L_WARNING, "[AUDIO] Tried to set rollof factor");
			// alSourcef(id, AL_ROLLOFF_FACTOR, ROLLOFF_FACTOR * curPlayingItem.rolloff * heightRolloffModifier);
		}

		if (!IsPlaying(true) || ((curPlayingItem.loopTime > 0) && (spring_gettime() > loopStop)))
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

	if (efxEnabled && (efxUpdates != efx.updates)) {
		// airAbsorption & LowPass aren't auto updated by OpenAL on change, so we need to do it per source
		// alSourcef(id, AL_AIR_ABSORPTION_FACTOR, efx.GetAirAbsorptionFactor());
		// alSourcei(id, AL_DIRECT_FILTER, efx.sfxFilter);
		// efxUpdates = efx.updates;
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

	if (curPlayingItem.id == 0)
		return INT_MIN;

	return (curPlayingItem.priority);
}

bool CSoundSource::IsPlaying(const bool checkOpenAl) const
{
	if (curStream)
		return true;

	if (asyncPlayItem.id != 0)
		return true;

	if (curPlayingItem.id == 0)
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
			item = sound->GetSoundItem(curPlayingItem.id);
		if (item != nullptr)
			item->StopPlay();

		curPlayingItem = {};
	}

	curStream.reset();

	if (curChannel != nullptr) {
		IAudioChannel* oldChannel = curChannel;
		curChannel = nullptr;
		oldChannel->SoundSourceFinished(this);
	}
	CheckError("CSoundSource::Stop");
}

void CSoundSource::Play(IAudioChannel* channel, SoundItem* item, float3 pos, float3 velocity, float volume, bool relative)
{
	assert(!curStream);
	assert(channel);

	if (!item->PlayNow())
		return;

    in3D = !relative && item->in3D;

    currentPosition = pos;

    name = item->name;

	const SoundBuffer& itemBuffer = SoundBuffer::GetById(item->GetSoundBufferID());

	Stop();

    name = item->name;
	curVolume = volume * item->GetGain() * channel->volume;
	curPlayingItem = {item->soundItemID,  item->loopTime, item->priority,  item->GetGain(), item->rolloff};
	curChannel = channel;

	alSourcei(id, AL_BUFFER, itemBuffer.GetId());
	alSourcef(id, AL_GAIN, curVolume);
	alSourcef(id, AL_PITCH, item->GetPitch() * globalPitch);

	velocity *= item->dopplerScale * ELMOS_TO_METERS;
	// alSource3f(id, AL_VELOCITY, velocity.x, velocity.y, velocity.z);
	alSourcei(id, AL_LOOPING, (item->loopTime > 0) ? AL_TRUE : AL_FALSE);

	loopStop = spring_gettime() + spring_msecs(item->loopTime);

	if (!in3D) {
		if (efxEnabled) {
			alSource3i(id, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
			alSourcei(id, AL_DIRECT_FILTER, AL_FILTER_NULL);
			efxEnabled = false;
		}
		alSourcei(id, AL_SOURCE_RELATIVE, AL_TRUE);
		alSourcef(id, AL_ROLLOFF_FACTOR, 0.f);
		alSource3f(id, AL_POSITION, 0.0f, 0.0f, -1.0f * ELMOS_TO_METERS);
#if defined(__APPLE__) || defined(__OpenBSD__)
		alSourcef(id, AL_REFERENCE_DISTANCE, REFERENCE_DIST * ELMOS_TO_METERS);
#endif
	} else {
		if (itemBuffer.GetChannels() > 1)
			LOG_L(L_WARNING, "Can not play non-mono \"%s\" in 3d.", itemBuffer.GetFilename().c_str());

		// if (efx.Enabled()) {
		// 	efxEnabled = true;
		// 	alSourcef(id, AL_AIR_ABSORPTION_FACTOR, efx.GetAirAbsorptionFactor());
		// 	alSource3i(id, AL_AUXILIARY_SEND_FILTER, efx.sfxSlot, 0, AL_FILTER_NULL);
		// 	alSourcei(id, AL_DIRECT_FILTER, efx.sfxFilter);
		// 	efxUpdates = efx.updates;
		// }

        alDopplerFactor(0);

		pos *= ELMOS_TO_METERS;

		alSourcei(id, AL_SOURCE_RELATIVE, AL_FALSE);
		alSource3f(id, AL_POSITION, pos.x, pos.y, pos.z);
		alSourcef(id, AL_ROLLOFF_FACTOR, 0);

        efx.Enable();
        if (attenuationFilter == 0) {
            alGenFilters(1, &attenuationFilter);
            alFilteri(attenuationFilter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
        }

        // need to set this here or else the filter state is incorrect when reusing a source
        alFilterf(attenuationFilter, AL_LOWPASS_GAIN, 1);
        alFilterf(attenuationFilter, AL_LOWPASS_GAINHF, 1);

        ComputeCameraSpaceData();
        ApplyGainBasedOnVisiblity(false);

        alSourcei(id, AL_DIRECT_FILTER, attenuationFilter);

        //we should not use attenuation features as they will fight against the nature of an rts game
        //it's necessary to calclulate the gain/filtering based on custom viewport related logic, not raw distance

		// curHeightRolloffModifier = heightRolloffModifier;
        // alSourcef(id, AL_ROLLOFF_FACTOR, ROLLOFF_FACTOR * item->rolloff * heightRolloffModifier);
		// alSourcef(id, AL_ROLLOFF_FACTOR, ROLLOFF_FACTOR);
        // alSourcef(id, AL_MAX_DISTANCE, MAX_DISTANCE * ELMOS_TO_METERS);

#if defined(__APPLE__) || defined(__OpenBSD__)
		alSourcef(id, AL_MAX_DISTANCE, 1000000.0f);
		// Max distance is too small by default on my Mac...
		ALfloat gain = channel->volume * item->GetGain() * volume;
		if (gain > 1.0f) {
			// OpenAL on Mac cannot handle AL_GAIN > 1 well, so we will adjust settings to get the same output with AL_GAIN = 1.
			const ALint model = alGetInteger(AL_DISTANCE_MODEL);
			const ALfloat rolloff = ROLLOFF_FACTOR * item->rolloff * heightRolloffModifier;
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

    alSourcePlay(id);

	if (itemBuffer.GetId() == 0)
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
	curChannel = channel;
	curVolume = volume;
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
	if (curChannel == nullptr)
		return;

	if (curStream) {
		alSourcef(id, AL_GAIN, curVolume * curChannel->volume);
		return;
	}
	if (curPlayingItem.id != 0) {
		alSourcef(id, AL_GAIN, curVolume * curPlayingItem.rndGain * curChannel->volume);
		return;
	}
}


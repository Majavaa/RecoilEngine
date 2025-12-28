/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include "System/Config/ConfigHandler.h"
#include "System/Sound/ISoundAttenuationModel.h"

class RtsAttenuationModel : public ISoundAttenuationModel
{
public:
    SoundAttenuationOutput Evaluate(const SoundAttenuationInput& in) const override;

private:
    float GetForwardAttenuationRange() const { return configHandler->GetFloat("snd_forwardAttenuationRange"); }
    float GetBackwardAttenuationRange() const { return configHandler->GetFloat("snd_backwardAttenuationRange"); }
    float GetOuterAttenuationRange() const { return configHandler->GetFloat("snd_outerAttenuationRange"); }
    float GetMinVolumeAttenuation() const { return configHandler->GetFloat("snd_minVolumeAttenuation"); }
    float GetMinFilterAttenuation() const { return configHandler->GetFloat("snd_minFilterAttenuation"); }
};

// "RTS Model: Designed for RTS games with viewport-based attenuation, forward/backward distance handling, off-screen attenuation, and zoom-aware center attenuation. Seen in games like \"Planetary Annihilation\" or \"Supreme Commander 2\""


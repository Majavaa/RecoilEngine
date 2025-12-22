/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include "System/Sound/ISoundAttenuationModel.h"

class RtsAttenuationModel : public ISoundAttenuationModel
{
public:
    SoundAttenuationOutput Evaluate(const SoundAttenuationInput& in) const override;

private:
    //TODO make these settings in the game when using the rts attenuation
    static constexpr float FORWARD_ATTENUATION_RANGE = 8000.0f;
    static constexpr float BACKWARD_ATTENUATION_RANGE = 300.0f;
    static constexpr float OUTER_ATTENUATION_RANGE = 1000.0f;
    static constexpr float OFFCENTER_SAFE_ZONE_RATIO = 0.3f;
    static constexpr float OFFCENTER_ATTENUATION_STRENGTH = 0.3f;
};

// "RTS Model: Designed for RTS games with viewport-based attenuation, forward/backward distance handling, off-screen attenuation, and zoom-aware center attenuation. Seen in games like \"Planetary Annihilation\" or \"Supreme Commander 2\""


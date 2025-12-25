#pragma once

#include "System/float3.h"
struct SoundAttenuationInput {
    float3 soundPosition;
    //maybe some sound specific settings like should it be resistant to attenuation
};

/// All of the data used to calculate the totalFactor as well as the final totalFactor
struct SoundAttenuationOutput {
    float forwardDistance;
    float innerDistance;
    float outerDistance;
    float frustumHeight;
    float frustumWidth;
    float volumeFactor;
    float filterFactor;
    float tiltFactor;
    float zoomFactor;
};

class ISoundAttenuationModel {
public:
    virtual ~ISoundAttenuationModel() = default;

    virtual SoundAttenuationOutput Evaluate(
        const SoundAttenuationInput& in
    ) const = 0;
};


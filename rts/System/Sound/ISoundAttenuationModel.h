#pragma once

struct SoundAttenuationInput {
    // float3 soundPosition;

    float forwardDistance;
    float innerDistance;
    float outerDistance;

    float viewportHalfWidth;
    float viewportHalfHeight;

    float zoomFactor;

    //maybe some sound specific settings like should it be resistant to attenuation
};

struct SoundAttenuationOutput {
    float totalFactor;
};

class ISoundAttenuationModel {
public:
    virtual ~ISoundAttenuationModel() = default;

    virtual SoundAttenuationOutput Evaluate(
        const SoundAttenuationInput& in
    ) const = 0;
};


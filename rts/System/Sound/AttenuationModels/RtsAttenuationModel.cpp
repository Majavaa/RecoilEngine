/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "RtsAttenuationModel.h"
#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "System/Sound/ISoundAttenuationModel.h"
#include "Game/TraceRay.h"
#include <algorithm>
#include <cmath>

SoundAttenuationOutput RtsAttenuationModel::Evaluate(const SoundAttenuationInput& in) const {

    SoundAttenuationOutput out;

    // -----------------------------------------------------------------
    // Convert the soundPosition in to camera space and calculate necessary data
    // -----------------------------------------------------------------

    {
        CCamera* playerCamera = CCameraHandler::GetCamera(CCamera::CAMTYPE_PLAYER);

        float terrainDistance = playerCamera->GetTerrainDistance();

        out.zoomFactor = 1 - std::clamp(terrainDistance == -1 ? 1.0f : terrainDistance / GetForwardAttenuationRange(), 0.0f, 1.0f);
        out.tiltFactor = playerCamera->GetForward().dot(float3(0.0f, -1.0f, 0.0f));

        float3 toSound = in.soundPosition - playerCamera->GetPos();

        float camForward = playerCamera->GetForward().dot(toSound);
        float camRight = playerCamera->GetRight().dot(toSound);
        float camUp = playerCamera->GetUp().dot(toSound);

        out.forwardDistance = camForward;

        float hfov = playerCamera->GetHFOV() * math::DEG_TO_RAD;
        float vfov = playerCamera->GetVFOV() * math::DEG_TO_RAD;

        float distance = std::abs(camForward);
        if (distance <= 0.0f) distance = 1.0f;

        out.frustumWidth = distance * std::tan(hfov * 0.5f);
        out.frustumHeight = distance * std::tan(vfov * 0.5f);

        float outsideRight = std::max(0.0f, std::abs(camRight) - out.frustumWidth);
        float outsideUp = std::max(0.0f, std::abs(camUp) - out.frustumHeight);

        out.outerDistance = std::sqrt(outsideRight * outsideRight + outsideUp * outsideUp);
    }

    // -----------------------------------------------------------------
    // Convert all the positional data to normalized ranges and calculate the final ranges
    // -----------------------------------------------------------------

    {
        // Calculate forward attenuation
        float forwardValue = std::clamp(out.forwardDistance >= 0 ?
            std::lerp(GetMinVolumeAttenuation(), 1.0f, 1.0f - out.forwardDistance / GetForwardAttenuationRange()) :
            std::clamp(1.0f - (-out.forwardDistance) / GetBackwardAttenuationRange(), 0.0f, 1.0f), GetMinVolumeAttenuation(), 1.0f);

        // Calculate outer attenuation
        float outerValue = std::clamp(std::lerp(GetMinFilterAttenuation(), 1.0f, 1.0f - out.outerDistance / GetOuterAttenuationRange()), GetMinFilterAttenuation(), 1.0f);

        out.volumeFactor = std::pow(forwardValue, 6) * outerValue;
        out.filterFactor = std::pow(forwardValue, 1) * outerValue;
    }

    return out;
}


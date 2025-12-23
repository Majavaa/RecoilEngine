/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "RtsAttenuationModel.h"
#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "System/Log/ILog.h"
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

        float3 toSound = in.soundPosition - playerCamera->GetPos();

        float camForward = playerCamera->GetForward().dot(toSound);
        float camRight = playerCamera->GetRight().dot(toSound);
        float camUp = playerCamera->GetUp().dot(toSound);

        out.innerDistance = sqrt(camRight * camRight + camUp * camUp);
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

        float terrainDistance = playerCamera->GetTerrainDistance();
        out.zoomFactor = std::clamp(terrainDistance == -1 ? 1.0f : terrainDistance / FORWARD_ATTENUATION_RANGE, 0.0f, 1.0f);
    }

    // -----------------------------------------------------------------
    // Convert all the positional data to normalized ranges and calculate the final range
    // -----------------------------------------------------------------

    {
        // Calculate forward attenuation
        float forwardValue = out.forwardDistance >= 0 ?
            std::clamp(1.0f - out.forwardDistance / FORWARD_ATTENUATION_RANGE, 0.0f, 1.0f) :
            std::clamp(1.0f - (-out.forwardDistance) / BACKWARD_ATTENUATION_RANGE, 0.0f, 1.0f);

        // Calculate outer attenuation
        float outerValue = std::clamp(1.0f - out.outerDistance / OUTER_ATTENUATION_RANGE, 0.0f, 1.0f);

        // Calculate inner attenuation
        float innerMaxRadius = std::min(out.frustumWidth, out.frustumHeight);
        float innerMinRadius = innerMaxRadius * OFFCENTER_SAFE_ZONE_RATIO;
        float t = std::clamp((out.innerDistance - innerMinRadius) / (innerMaxRadius - innerMinRadius), 0.0f, 1.0f);

        // Apply zoom factor to inner attenuation
        float innerValue = 1.0f - t * (OFFCENTER_ATTENUATION_STRENGTH * out.zoomFactor);

        // Combine all attenuation factors
        out.totalFactor = forwardValue * outerValue * innerValue;
    }

    return out;
}


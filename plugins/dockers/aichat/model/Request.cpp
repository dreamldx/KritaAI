/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Request.h"

namespace AiChat {

RequestSettings makeRequestSettings(const Config &config, ReasoningEffort effort)
{
    RequestSettings request;
    request.url = config.chatCompletionsUrl();
    request.apiKey = config.apiKey;
    request.model = config.model;
    if (config.hasVision()) {
        request.vision.url = Config::chatCompletionsUrl(config.visionBaseUrl);
        request.vision.apiKey = config.visionApiKey;
        request.vision.model = config.visionModel.trimmed();
        request.vision.disableThinking = config.visionDisableThinking;
        request.vision.ignoreProviders = config.visionIgnoredProviders();
    }
    request.fal.apiKey = config.falApiKey.trimmed();
    if (config.sendReasoningEffort) {
        switch (effort) {
        case ReasoningEffort::Low:
            request.reasoningEffort = QStringLiteral("low");
            break;
        case ReasoningEffort::High:
            request.reasoningEffort = QStringLiteral("high");
            break;
        case ReasoningEffort::Medium:
        default:
            request.reasoningEffort = QStringLiteral("medium");
            break;
        }
    }
    return request;
}

} // namespace AiChat

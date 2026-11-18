/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AiChatConfig.h"

#include <kis_config.h>

namespace {
const QString kBaseUrlKey = QStringLiteral("aichat_base_url");
const QString kApiKeyKey = QStringLiteral("aichat_api_key");
const QString kModelKey = QStringLiteral("aichat_model");
const QString kSendEffortKey = QStringLiteral("aichat_send_reasoning_effort");
const QString kVisionUrlKey = QStringLiteral("aichat_vision_base_url");
const QString kVisionKeyKey = QStringLiteral("aichat_vision_api_key");
const QString kVisionModelKey = QStringLiteral("aichat_vision_model");
const QString kVisionNoThinkKey = QStringLiteral("aichat_vision_disable_thinking");
const QString kChatPath =QStringLiteral("/chat/completions");
} // namespace

AiChatConfig AiChatConfig::defaults()
{
    AiChatConfig config;
    config.baseUrl = QStringLiteral("https://api.openai.com/v1");
    config.model = QStringLiteral("gpt-4o-mini");
    return config;
}

AiChatConfig AiChatConfig::load()
{
    const AiChatConfig fallback = defaults();
    KisConfig cfg(true);

    AiChatConfig config;
    config.baseUrl = cfg.readEntry<QString>(kBaseUrlKey, fallback.baseUrl);
    config.apiKey = cfg.readEntry<QString>(kApiKeyKey, fallback.apiKey);
    config.model = cfg.readEntry<QString>(kModelKey, fallback.model);
    config.sendReasoningEffort = cfg.readEntry<bool>(kSendEffortKey, fallback.sendReasoningEffort);
    config.visionBaseUrl = cfg.readEntry<QString>(kVisionUrlKey, fallback.visionBaseUrl);
    config.visionApiKey = cfg.readEntry<QString>(kVisionKeyKey, fallback.visionApiKey);
    config.visionModel = cfg.readEntry<QString>(kVisionModelKey, fallback.visionModel);
    config.visionDisableThinking = cfg.readEntry<bool>(kVisionNoThinkKey, fallback.visionDisableThinking);
    return config;
}

void AiChatConfig::save() const
{
    KisConfig cfg(false);
    cfg.writeEntry(kBaseUrlKey, baseUrl.trimmed());
    cfg.writeEntry(kApiKeyKey, apiKey.trimmed());
    cfg.writeEntry(kModelKey, model.trimmed());
    cfg.writeEntry(kSendEffortKey, sendReasoningEffort);
    cfg.writeEntry(kVisionUrlKey, visionBaseUrl.trimmed());
    cfg.writeEntry(kVisionKeyKey, visionApiKey.trimmed());
    cfg.writeEntry(kVisionModelKey, visionModel.trimmed());
    cfg.writeEntry(kVisionNoThinkKey, visionDisableThinking);
}

bool AiChatConfig::hasVision() const
{
    return chatCompletionsUrl(visionBaseUrl).isValid() && !visionModel.trimmed().isEmpty();
}

QUrl AiChatConfig::chatCompletionsUrl() const
{
    return chatCompletionsUrl(baseUrl);
}

QUrl AiChatConfig::chatCompletionsUrl(const QString &baseUrl)
{
    QString text = baseUrl.trimmed();
    if (text.isEmpty()) {
        return QUrl();
    }

    while (text.endsWith(QLatin1Char('/'))) {
        text.chop(1);
    }
    if (!text.endsWith(kChatPath)) {
        text += kChatPath;
    }

    const QUrl url(text, QUrl::StrictMode);
    const bool usable = url.isValid() && !url.host().isEmpty()
        && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"));
    return usable ? url : QUrl();
}

bool AiChatConfig::isComplete() const
{
    return chatCompletionsUrl().isValid() && !model.trimmed().isEmpty();
}

/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Config.h"

#include <QRegularExpression>

#include <kis_config.h>

namespace AiChat {

namespace {
const QString kBaseUrlKey = QStringLiteral("aichat_base_url");
const QString kApiKeyKey = QStringLiteral("aichat_api_key");
const QString kModelKey = QStringLiteral("aichat_model");
const QString kSendEffortKey = QStringLiteral("aichat_send_reasoning_effort");
const QString kVisionUrlKey = QStringLiteral("aichat_vision_base_url");
const QString kVisionKeyKey = QStringLiteral("aichat_vision_api_key");
const QString kVisionModelKey = QStringLiteral("aichat_vision_model");
const QString kVisionNoThinkKey = QStringLiteral("aichat_vision_disable_thinking");
const QString kVisionBlacklistKey = QStringLiteral("aichat_vision_provider_blacklist");
const QString kFalKeyKey = QStringLiteral("aichat_fal_api_key");
const QString kChatPath = QStringLiteral("/chat/completions");
} // namespace

Config Config::defaults()
{
    Config config;
    config.baseUrl = QStringLiteral("https://api.openai.com/v1");
    config.model = QStringLiteral("gpt-4o-mini");
    return config;
}

Config Config::load()
{
    const Config fallback = defaults();
    KisConfig cfg(true);

    Config config;
    config.baseUrl = cfg.readEntry<QString>(kBaseUrlKey, fallback.baseUrl);
    config.apiKey = cfg.readEntry<QString>(kApiKeyKey, fallback.apiKey);
    config.model = cfg.readEntry<QString>(kModelKey, fallback.model);
    config.sendReasoningEffort = cfg.readEntry<bool>(kSendEffortKey, fallback.sendReasoningEffort);
    config.visionBaseUrl = cfg.readEntry<QString>(kVisionUrlKey, fallback.visionBaseUrl);
    config.visionApiKey = cfg.readEntry<QString>(kVisionKeyKey, fallback.visionApiKey);
    config.visionModel = cfg.readEntry<QString>(kVisionModelKey, fallback.visionModel);
    config.visionDisableThinking = cfg.readEntry<bool>(kVisionNoThinkKey, fallback.visionDisableThinking);
    config.visionProviderBlacklist = cfg.readEntry<QString>(kVisionBlacklistKey, fallback.visionProviderBlacklist);
    config.falApiKey = cfg.readEntry<QString>(kFalKeyKey, fallback.falApiKey);
    return config;
}

void Config::save() const
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
    cfg.writeEntry(kVisionBlacklistKey, visionProviderBlacklist.trimmed());
    cfg.writeEntry(kFalKeyKey, falApiKey.trimmed());
}

QStringList Config::parseProviderList(const QString &text)
{
    QStringList result;
    for (const QString &part : text.split(QRegularExpression(QStringLiteral("[,;\\n\\r]")), Qt::SkipEmptyParts)) {
        const QString name = part.trimmed();
        // names are compared the way OpenRouter does: not by case
        if (!name.isEmpty() && !result.contains(name, Qt::CaseInsensitive)) {
            result << name;
        }
    }
    return result;
}

QStringList Config::visionIgnoredProviders() const
{
    return parseProviderList(visionProviderBlacklist);
}

bool Config::hasVision() const
{
    return chatCompletionsUrl(visionBaseUrl).isValid() && !visionModel.trimmed().isEmpty();
}

QUrl Config::chatCompletionsUrl() const
{
    return chatCompletionsUrl(baseUrl);
}

QUrl Config::chatCompletionsUrl(const QString &baseUrl)
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

bool Config::isComplete() const
{
    return chatCompletionsUrl().isValid() && !model.trimmed().isEmpty();
}

} // namespace AiChat

/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _CONFIG_H_
#define _CONFIG_H_

#include <QString>
#include <QStringList>
#include <QUrl>

namespace AiChat {

/**
 * The user's AI service settings. They live in Krita's configuration file
 * (kritarc) and are edited on the "AI Chat" page of Configure Krita.
 *
 * The API key is stored unencrypted, like every other Krita setting.
 */
struct Config {
    QString baseUrl; ///< e.g. "https://api.openai.com/v1"
    QString apiKey;
    QString model;
    bool sendReasoningEffort {false}; ///< many OpenAI-compatible servers reject "reasoning_effort"

    /// The picture-reading model behind the "ask_about_image" tool (e.g. DeepSeek Flash with vision).
    /// Empty address or model: the tool is not offered to the AI.
    QString visionBaseUrl;
    QString visionApiKey;
    QString visionModel;
    bool visionDisableThinking {true}; ///< ask the vision model not to think ("reasoning": {"enabled": false})
    QString visionProviderBlacklist; ///< as typed: names separated by commas, semicolons or lines
    bool hasVision() const;

    /// The blacklist as a list: trimmed, without empty entries and without doubles.
    QStringList visionIgnoredProviders() const;
    static QStringList parseProviderList(const QString &text);

    /// fal.ai key for the Topaz tools (upscale, denoise). Empty: the tools are not offered.
    QString falApiKey;

    static Config defaults();
    static Config load();
    void save() const;

    /// The ".../chat/completions" URL for this configuration; invalid if the base URL is unusable.
    QUrl chatCompletionsUrl() const;
    static QUrl chatCompletionsUrl(const QString &baseUrl);

    /// True if there is enough to send a request (a usable URL and a model name).
    bool isComplete() const;
};

} // namespace AiChat

#endif

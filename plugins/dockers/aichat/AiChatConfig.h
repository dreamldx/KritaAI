/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_CHAT_CONFIG_H_
#define _AI_CHAT_CONFIG_H_

#include <QString>
#include <QUrl>

/**
 * The user's AI service settings. They live in Krita's configuration file
 * (kritarc) and are edited on the "AI Chat" page of Configure Krita.
 *
 * The API key is stored unencrypted, like every other Krita setting.
 */
struct AiChatConfig {
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
    bool hasVision() const;

    static AiChatConfig defaults();
    static AiChatConfig load();
    void save() const;

    /// The ".../chat/completions" URL for this configuration; invalid if the base URL is unusable.
    QUrl chatCompletionsUrl() const;
    static QUrl chatCompletionsUrl(const QString &baseUrl);

    /// True if there is enough to send a request (a usable URL and a model name).
    bool isComplete() const;
};

#endif

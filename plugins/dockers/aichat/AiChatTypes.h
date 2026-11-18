/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_CHAT_TYPES_H_
#define _AI_CHAT_TYPES_H_

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QUrl>

/// A chat message as it is shown in the panel and kept in the database.
struct AiChatStoredMessage {
    qint64 id {-1};
    QString role; ///< "user" or "assistant"
    QString content;
    QDateTime createdAt;
    qint64 processingMs {-1}; ///< assistant messages only: how long the AI needed, -1 if unknown
    qint64 tokens {-1}; ///< assistant messages only: tokens the request(s) used, -1 if the server did not say
};
Q_DECLARE_METATYPE(AiChatStoredMessage)
Q_DECLARE_METATYPE(QList<AiChatStoredMessage>)

/// One function the model asked us to run.
struct AiChatToolCall {
    QString id;
    QString name;
    QString arguments; ///< JSON text, exactly as the model produced it
};

/// The model that looks at pictures for the "ask_about_image" tool. May be a different
/// server than the chat model, so the chat model itself does not need to see images.
struct AiChatVisionSettings {
    QUrl url; ///< the complete ".../chat/completions" URL; invalid: the tool is not offered
    QString apiKey;
    QString model;
    bool disableThinking {true}; ///< sends "reasoning": {"enabled": false}: looking at a picture needs no long thinking

    bool isValid() const { return url.isValid() && !model.isEmpty(); }
};

/// Everything the HTTP client needs to know about the server. Plain data, safe to pass between threads.
struct AiChatRequestSettings {
    QUrl url; ///< the complete ".../chat/completions" URL
    QString apiKey; ///< empty: no Authorization header (local servers)
    QString model;
    QString reasoningEffort; ///< "low", "medium", "high"; empty: the field is not sent
    AiChatVisionSettings vision;
};
Q_DECLARE_METATYPE(AiChatRequestSettings)

#endif

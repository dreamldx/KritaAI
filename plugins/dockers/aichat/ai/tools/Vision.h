/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_TOOLS_VISION_H_
#define _AI_TOOLS_VISION_H_

#include <QByteArray>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <atomic>

class QEventLoop;

namespace AiChat {

/// The model that looks at pictures for the "ask_about_image" tool. May be a different
/// server than the chat model, so the chat model itself does not need to see images.
struct VisionSettings {
    QUrl url; ///< the complete ".../chat/completions" URL; invalid: the tool is not offered
    QString apiKey;
    QString model;
    bool disableThinking {true}; ///< sends "reasoning": {"enabled": false}: looking at a picture needs no long thinking
    QStringList ignoreProviders; ///< OpenRouter only: sent as "provider": {"ignore": [...]}, these companies get no request

    bool isValid() const { return url.isValid() && !model.isEmpty(); }
};


/**
 * Asks a vision model a question about a picture.
 *
 * The chat model of the conversation does not have to see images: it calls the
 * "ask_about_image" tool, the picture goes to this (possibly different) model,
 * and only its text answer goes back. The picture never enters the conversation,
 * so it is not stored and does not use up the context.
 *
 * ask() blocks the calling thread (it runs a local event loop) until the answer
 * is there, the request failed, cancel() was called or `timeoutMs` passed. Use it
 * in the worker thread. cancel() may be called from any thread.
 */
class VisionClient : public QObject
{
    Q_OBJECT
public:
    explicit VisionClient(QObject *parent = nullptr);

    /// Returns the answer, or the reason in `*error` (and an empty string).
    QString ask(const VisionSettings &settings, const QString &question, const QByteArray &imageData,
                const QString &imageMimeType, QString *error, int timeoutMs = 120000);

    /// Makes a running ask() return with an error. Does nothing if none is running.
    void cancel();

    /// The request body: one user message with the question and the picture as a data URI.
    /// With `disableThinking` the body also has "reasoning": {"enabled": false} (the OpenRouter way to
    /// switch the thinking of a model off); without it the model may think for many seconds.
    /// `ignoreProviders` (OpenRouter) becomes "provider": {"ignore": [...]}: those companies are not asked.
    static QByteArray buildRequestBody(const QString &model, const QString &question, const QByteArray &imageData,
                                       const QString &imageMimeType, bool disableThinking = false,
                                       const QStringList &ignoreProviders = QStringList());

    /// The text of a chat-completions answer; empty (with `*error`) if the answer has none.
    static QString parseAnswer(const QByteArray &body, QString *error);

    /// A QImage as the bytes of a PNG file.
    static QByteArray encodePng(const QImage &image);

private:
    QPointer<QNetworkReply> m_reply;
    std::atomic<QEventLoop *> m_loop {nullptr};
    std::atomic<bool> m_canceled {false};
};

} // namespace AiChat

#endif

/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_CHAT_VISION_H_
#define _AI_CHAT_VISION_H_

#include <QByteArray>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>

#include <atomic>

#include "AiChatTypes.h"

class QEventLoop;

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
class AiChatVisionClient : public QObject
{
    Q_OBJECT
public:
    explicit AiChatVisionClient(QObject *parent = nullptr);

    /// Returns the answer, or the reason in `*error` (and an empty string).
    QString ask(const AiChatVisionSettings &settings, const QString &question, const QByteArray &imageData,
                const QString &imageMimeType, QString *error, int timeoutMs = 120000);

    /// Makes a running ask() return with an error. Does nothing if none is running.
    void cancel();

    /// The request body: one user message with the question and the picture as a data URI.
    /// With `disableThinking` the body also has "reasoning": {"enabled": false} (the OpenRouter way to
    /// switch the thinking of a model off); without it the model may think for many seconds.
    static QByteArray buildRequestBody(const QString &model, const QString &question, const QByteArray &imageData,
                                       const QString &imageMimeType, bool disableThinking = false);

    /// The text of a chat-completions answer; empty (with `*error`) if the answer has none.
    static QString parseAnswer(const QByteArray &body, QString *error);

    /// A QImage as the bytes of a PNG file.
    static QByteArray encodePng(const QImage &image);

private:
    QPointer<QNetworkReply> m_reply;
    std::atomic<QEventLoop *> m_loop {nullptr};
    std::atomic<bool> m_canceled {false};
};

#endif

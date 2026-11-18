/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AiChatVision.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QTimer>

#include <klocalizedstring.h>

#include "AiChatLog.h"
#include "AiChatStream.h"

AiChatVisionClient::AiChatVisionClient(QObject *parent)
    : QObject(parent)
{
}

QByteArray AiChatVisionClient::encodePng(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QByteArray AiChatVisionClient::buildRequestBody(const QString &model, const QString &question, const QByteArray &imageData,
                                                const QString &imageMimeType, bool disableThinking)
{
    const QString dataUri = QStringLiteral("data:%1;base64,%2").arg(imageMimeType, QString::fromLatin1(imageData.toBase64()));

    QJsonObject textPart;
    textPart["type"] = "text";
    textPart["text"] = question;

    QJsonObject imageUrl;
    imageUrl["url"] = dataUri;
    QJsonObject imagePart;
    imagePart["type"] = "image_url";
    imagePart["image_url"] = imageUrl;

    QJsonObject message;
    message["role"] = "user";
    message["content"] = QJsonArray {textPart, imagePart};

    QJsonObject body;
    body["model"] = model;
    body["messages"] = QJsonArray {message};
    body["stream"] = false;
    if (disableThinking) {
        QJsonObject reasoning;
        reasoning["enabled"] = false;
        body["reasoning"] = reasoning;
    }
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QString AiChatVisionClient::parseAnswer(const QByteArray &body, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = i18n("The vision model sent an answer that is not JSON.");
        return QString();
    }

    const QJsonArray choices = document.object().value("choices").toArray();
    if (choices.isEmpty()) {
        *error = i18n("The vision model sent no answer.");
        return QString();
    }

    // "content" is a string; a few servers send a list of parts instead
    const QJsonValue content = choices.first().toObject().value("message").toObject().value("content");
    QString text;
    if (content.isString()) {
        text = content.toString();
    } else if (content.isArray()) {
        for (const QJsonValue &part : content.toArray()) {
            text += part.toObject().value("text").toString();
        }
    }

    if (text.trimmed().isEmpty()) {
        *error = i18n("The vision model sent an empty answer.");
        return QString();
    }
    return text.trimmed();
}

QString AiChatVisionClient::ask(const AiChatVisionSettings &settings, const QString &question, const QByteArray &imageData,
                                const QString &imageMimeType, QString *error, int timeoutMs)
{
    if (!settings.isValid()) {
        *error = i18n("No vision model is set up. Open Settings > Configure Krita > AI Chat.");
        return QString();
    }

    m_canceled = false;

    // Created here, so it belongs to the thread that calls ask().
    QNetworkAccessManager manager;

    QNetworkRequest request(settings.url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!settings.apiKey.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + settings.apiKey.toUtf8());
    }

    const QByteArray requestBody = buildRequestBody(settings.model, question, imageData, imageMimeType, settings.disableThinking);
    QElapsedTimer requestTimer;
    requestTimer.start();
    qint64 firstByteMs = -1;

    QNetworkReply *reply = manager.post(request, requestBody);
    m_reply = reply;
    connect(reply, &QNetworkReply::readyRead, reply, [&]() {
        if (firstByteMs < 0) {
            firstByteMs = requestTimer.elapsed();
        }
    });

    QEventLoop loop;
    m_loop = &loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool timedOut = false;
    connect(&timeout, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        loop.quit();
    });
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(timeoutMs);
    if (!reply->isFinished() && !m_canceled) {
        loop.exec();
    }
    m_loop = nullptr;

    QString answer;
    QString outcome;
    QString usage;
    QString provider;
    if (m_canceled) {
        *error = i18n("Canceled.");
        outcome = QStringLiteral("canceled");
    } else if (timedOut) {
        *error = i18n("The vision model did not answer in time.");
        outcome = QStringLiteral("timeout");
    } else {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        outcome = QStringLiteral("HTTP %1").arg(status);
        // "usage" tells how many tokens the model spent, also on thinking ("reasoning_tokens")
        const QJsonObject bodyObject = QJsonDocument::fromJson(body).object();
        usage = QString::fromUtf8(QJsonDocument(bodyObject.value("usage").toObject()).toJson(QJsonDocument::Compact));
        // OpenRouter says which company served the request; a slow request is often a slow provider
        provider = bodyObject.value("provider").toString();
        if (status >= 400) {
            *error = aiChatHttpErrorMessage(status, body);
        } else if (reply->error() != QNetworkReply::NoError) {
            *error = reply->errorString();
        } else {
            answer = parseAnswer(body, error);
        }
    }

    // abort() would emit finished() again; nothing listens any more
    reply->disconnect();
    if (!reply->isFinished()) {
        reply->abort();
    }
    reply->deleteLater();
    m_reply = nullptr;

    aiChatLog(QStringLiteral("vision request: model=%1 provider=%8 sent=%2 KB, first byte after %3 ms, finished after %4 ms, %5, answer=%6 chars, usage=%7")
                  .arg(settings.model)
                  .arg(requestBody.size() / 1024)
                  .arg(firstByteMs)
                  .arg(requestTimer.elapsed())
                  .arg(outcome)
                  .arg(answer.size())
                  .arg(usage.isEmpty() ? QStringLiteral("-") : usage)
                  .arg(provider.isEmpty() ? QStringLiteral("?") : provider));
    return answer;
}

void AiChatVisionClient::cancel()
{
    m_canceled = true;
    if (QEventLoop *loop = m_loop.load()) {
        loop->quit(); // thread-safe
    }
}

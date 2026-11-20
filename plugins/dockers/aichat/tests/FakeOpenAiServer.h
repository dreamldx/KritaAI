/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

// A small HTTP server that pretends to be an OpenAI compatible service, for the tests of the AI
// chat. It runs inside the test (no real service is needed): the test scripts the answers and
// afterwards looks at what was sent.
//
// JSON is built in code and not written as raw string literals: moc 5.15 gives up on a file that
// has a raw string with an escaped quote in it.

#ifndef _AI_CHAT_FAKE_SERVER_H_
#define _AI_CHAT_FAKE_SERVER_H_

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QSharedPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

namespace {

// ---- a scripted fake "OpenAI" server ----------------------------------------------------

struct ScriptedResponse {
    int status {200};
    QByteArray contentType {"text/event-stream"};
    QList<QByteArray> chunks;
    int firstDelayMs {0}; ///< wait this long before answering
    int chunkDelayMs {0}; ///< wait this long between the chunks
    bool hangAfterChunks {false}; ///< never end the response (to test cancel and shutdown)
};

struct RecordedRequest {
    QByteArray path;
    QMap<QByteArray, QByteArray> headers; ///< names in lower case
    QByteArray body;

    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
    QJsonArray messages() const { return json().value("messages").toArray(); }
};

class FakeServer
{
public:
    bool start()
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this]() {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                accept(socket);
            }
        });
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    QUrl chatUrl() const { return QUrl(QString("http://127.0.0.1:%1/v1/chat/completions").arg(m_server.serverPort())); }

    QUrl baseUrl() const { return QUrl(QString("http://127.0.0.1:%1").arg(m_server.serverPort())); }

    void enqueue(const ScriptedResponse &response) { m_script.append(response); }

    QList<RecordedRequest> requests;

private:
    void accept(QTcpSocket *socket)
    {
        auto buffer = QSharedPointer<QByteArray>::create();
        auto handled = QSharedPointer<bool>::create(false);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer, handled]() {
            buffer->append(socket->readAll());
            if (*handled) {
                return;
            }

            const int headerEnd = buffer->indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                return;
            }

            RecordedRequest request;
            const QList<QByteArray> lines = buffer->left(headerEnd).split('\n');
            request.path = lines.first().split(' ').value(1);
            int contentLength = 0;
            for (int i = 1; i < lines.size(); ++i) {
                const int colon = lines[i].indexOf(':');
                if (colon > 0) {
                    const QByteArray name = lines[i].left(colon).trimmed().toLower();
                    request.headers[name] = lines[i].mid(colon + 1).trimmed();
                }
            }
            contentLength = request.headers.value("content-length").toInt();

            if (buffer->size() < headerEnd + 4 + contentLength) {
                return; // the body is not complete yet
            }
            *handled = true;
            request.body = buffer->mid(headerEnd + 4, contentLength);
            requests.append(request);

            respond(socket, m_script.isEmpty() ? errorResponse() : m_script.takeFirst());
        });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }

    static ScriptedResponse errorResponse()
    {
        ScriptedResponse response;
        response.status = 500;
        response.contentType = "text/plain";
        response.chunks = {"the test did not script a response for this request"};
        return response;
    }

    void respond(QTcpSocket *socket, const ScriptedResponse &response)
    {
        QPointer<QTcpSocket> guard(socket);
        QTimer::singleShot(response.firstDelayMs, socket, [guard, response]() {
            if (!guard) {
                return;
            }

            const bool streamed = response.contentType.contains("event-stream");
            QByteArray head = "HTTP/1.1 " + QByteArray::number(response.status) + " X\r\n";
            head += "Content-Type: " + response.contentType + "\r\n";
            head += "Connection: close\r\n";
            if (!streamed) {
                int length = 0;
                for (const QByteArray &chunk : response.chunks) {
                    length += chunk.size();
                }
                head += "Content-Length: " + QByteArray::number(length) + "\r\n";
            }
            head += "\r\n";
            guard->write(head);

            writeChunks(guard, response, 0);
        });
    }

    static void writeChunks(QPointer<QTcpSocket> socket, const ScriptedResponse &response, int index)
    {
        if (!socket) {
            return;
        }
        if (index >= response.chunks.size()) {
            if (!response.hangAfterChunks) {
                socket->disconnectFromHost();
            }
            return;
        }

        socket->write(response.chunks[index]);
        socket->flush();
        QTimer::singleShot(response.chunkDelayMs, socket, [socket, response, index]() {
            writeChunks(socket, response, index + 1);
        });
    }

    QTcpServer m_server;
    QList<ScriptedResponse> m_script;
};

// ---- building the server's answers ------------------------------------------------------

QByteArray sse(const QJsonObject &object)
{
    return "data: " + QJsonDocument(object).toJson(QJsonDocument::Compact) + "\n\n";
}

QJsonObject chunkWithDelta(const QJsonObject &delta, const QString &finishReason = QString())
{
    QJsonObject choice;
    choice["delta"] = delta;
    choice["finish_reason"] = finishReason.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(finishReason);
    QJsonObject chunk;
    chunk["choices"] = QJsonArray{choice};
    return chunk;
}

QByteArray textChunk(const QString &text)
{
    QJsonObject delta;
    delta["content"] = text;
    return sse(chunkWithDelta(delta));
}

QByteArray finishChunk(const QString &reason = "stop")
{
    return sse(chunkWithDelta(QJsonObject(), reason));
}

QByteArray toolCallChunk(int index, const QString &id, const QString &name, const QString &arguments)
{
    QJsonObject function;
    if (!name.isEmpty()) {
        function["name"] = name;
    }
    function["arguments"] = arguments;
    QJsonObject call;
    call["index"] = index;
    if (!id.isEmpty()) {
        call["id"] = id;
        call["type"] = "function";
    }
    call["function"] = function;
    QJsonObject delta;
    delta["tool_calls"] = QJsonArray{call};
    return sse(chunkWithDelta(delta));
}

/// The last chunk of a streamed answer when "stream_options": {"include_usage": true} was asked for:
/// no choices, only the token counts.
QByteArray usageChunk(int promptTokens, int completionTokens)
{
    QJsonObject usage;
    usage["prompt_tokens"] = promptTokens;
    usage["completion_tokens"] = completionTokens;
    usage["total_tokens"] = promptTokens + completionTokens;
    QJsonObject chunk;
    chunk["choices"] = QJsonArray();
    chunk["usage"] = usage;
    return sse(chunk);
}

QByteArray doneEvent()
{
    return "data: [DONE]\n\n";
}

/// A complete streamed answer with the given text.
ScriptedResponse textReply(const QString &text)
{
    ScriptedResponse response;
    response.chunks = {textChunk(text), finishChunk(), doneEvent()};
    return response;
}

/// An answer in which the model asks for the "paint" tool.
ScriptedResponse paintToolCallReply(const QString &id = "call_1")
{
    ScriptedResponse response;
    response.chunks = {
        toolCallChunk(0, id, "paint", ""),
        toolCallChunk(0, "", "", "{\"color\":"),
        toolCallChunk(0, "", "", "\"red\"}"),
        finishChunk("tool_calls"),
        doneEvent(),
    };
    return response;
}

/// An answer that starts with some text and then never ends.
ScriptedResponse hangingReply(const QString &firstText)
{
    ScriptedResponse response;
    response.chunks = {textChunk(firstText)};
    response.hangAfterChunks = true;
    return response;
}


} // namespace

#endif

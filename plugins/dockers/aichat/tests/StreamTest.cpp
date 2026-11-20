/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include "../config/Config.h"
#include "../network/Stream.h"

using namespace AiChat;

class StreamTest : public QObject
{
    Q_OBJECT

private:
    static QJsonObject json(const char *text)
    {
        return QJsonDocument::fromJson(QByteArray(text)).object();
    }

    // Built in code instead of as raw string literals: moc 5.15 gives up on a file that has
    // a raw string with an escaped quote in it.

    /// One streamed chunk that carries a piece of a tool call.
    static QJsonObject toolCallChunk(int index, const QString &id, const QString &name, const QString &arguments)
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
        QJsonObject choice;
        choice["delta"] = delta;
        QJsonObject chunk;
        chunk["choices"] = QJsonArray{choice};
        return chunk;
    }

    /// A complete, non-streamed response that asks for one tool call.
    static QJsonObject fullToolCallResponse(const QString &id, const QString &name, const QString &arguments)
    {
        QJsonObject function;
        function["name"] = name;
        function["arguments"] = arguments;
        QJsonObject call;
        call["id"] = id;
        call["type"] = "function";
        call["function"] = function;

        QJsonObject message;
        message["role"] = "assistant";
        message["content"] = QJsonValue::Null;
        message["tool_calls"] = QJsonArray{call};
        QJsonObject choice;
        choice["message"] = message;
        choice["finish_reason"] = "tool_calls";
        QJsonObject response;
        response["choices"] = QJsonArray{choice};
        return response;
    }

private Q_SLOTS:
    // ---- URL building -------------------------------------------------

    void testChatCompletionsUrl_data()
    {
        QTest::addColumn<QString>("base");
        QTest::addColumn<QString>("expected");

        QTest::newRow("plain") << "https://api.openai.com/v1" << "https://api.openai.com/v1/chat/completions";
        QTest::newRow("trailing slash") << "https://api.openai.com/v1/" << "https://api.openai.com/v1/chat/completions";
        QTest::newRow("spaces") << "  http://localhost:11434/v1  " << "http://localhost:11434/v1/chat/completions";
        QTest::newRow("already complete") << "https://x.example/v1/chat/completions" << "https://x.example/v1/chat/completions";
        QTest::newRow("empty") << "" << "";
        QTest::newRow("no scheme") << "api.openai.com/v1" << "";
        QTest::newRow("wrong scheme") << "ftp://example.com/v1" << "";
        QTest::newRow("garbage") << "not a url" << "";
    }

    void testChatCompletionsUrl()
    {
        QFETCH(QString, base);
        QFETCH(QString, expected);
        const QUrl url = Config::chatCompletionsUrl(base);
        if (expected.isEmpty()) {
            QVERIFY(!url.isValid());
        } else {
            QCOMPARE(url.toString(), expected);
        }
    }

    // ---- SSE parser ---------------------------------------------------

    void testSseSingleEvent()
    {
        SseParser parser;
        QCOMPARE(parser.feed("data: hello\n\n"), QStringList{"hello"});
    }

    void testSseSplitAcrossChunks()
    {
        SseParser parser;
        QVERIFY(parser.feed("da").isEmpty());
        QVERIFY(parser.feed("ta: {\"a\":").isEmpty());
        QVERIFY(parser.feed("1}\n").isEmpty()); // line done, event not yet
        QCOMPARE(parser.feed("\n"), QStringList{"{\"a\":1}"});
    }

    void testSseSeveralEventsInOneChunk()
    {
        SseParser parser;
        const QStringList events = parser.feed("data: one\n\ndata: two\n\ndata: [DONE]\n\n");
        QCOMPARE(events, (QStringList{"one", "two", "[DONE]"}));
    }

    void testSseCrLfAndComments()
    {
        SseParser parser;
        const QStringList events = parser.feed(": keep-alive\r\ndata: x\r\n\r\n");
        QCOMPARE(events, QStringList{"x"});
    }

    void testSseMultiLineData()
    {
        SseParser parser;
        QCOMPARE(parser.feed("data: a\ndata: b\n\n"), QStringList{"a\nb"});
    }

    void testSseIgnoresOtherFields()
    {
        SseParser parser;
        QCOMPARE(parser.feed("event: message\nid: 7\ndata: x\n\n"), QStringList{"x"});
    }

    void testSseNoSpaceAfterColon()
    {
        SseParser parser;
        QCOMPARE(parser.feed("data:x\n\n"), QStringList{"x"});
    }

    void testSseMultiByteCharacterSplitAcrossChunks()
    {
        const QByteArray full = QString("data: 你好\n\n").toUtf8();
        SseParser parser;
        // cut in the middle of a three-byte character
        const int cut = full.indexOf(QByteArray("\xe5\xa5\xbd")) + 1;
        QVERIFY(parser.feed(full.left(cut)).isEmpty());
        QCOMPARE(parser.feed(full.mid(cut)), QStringList{QString("你好")});
    }

    void testSseFlushDispatchesUnterminatedEvent()
    {
        SseParser parser;
        QVERIFY(parser.feed("data: tail").isEmpty());
        QCOMPARE(parser.flush(), QStringList{"tail"});
        QVERIFY(parser.flush().isEmpty());
    }

    // ---- completion accumulator --------------------------------------

    void testAccumulatesText()
    {
        CompletionAccumulator acc;
        QCOMPARE(acc.addChunk(json(R"({"choices":[{"delta":{"role":"assistant","content":""}}]})")), QString());
        QCOMPARE(acc.addChunk(json(R"({"choices":[{"delta":{"content":"Hel"}}]})")), QString("Hel"));
        QCOMPARE(acc.addChunk(json(R"({"choices":[{"delta":{"content":"lo"}}]})")), QString("lo"));
        QCOMPARE(acc.addChunk(json(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})")), QString());

        QCOMPARE(acc.text(), QString("Hello"));
        QCOMPARE(acc.finishReason(), QString("stop"));
        QVERIFY(!acc.hasError());
        QVERIFY(acc.toolCalls().isEmpty());
    }

    void testNullContentAndEmptyChoices()
    {
        CompletionAccumulator acc;
        QCOMPARE(acc.addChunk(json(R"({"choices":[{"delta":{"content":null}}]})")), QString());
        QCOMPARE(acc.addChunk(json(R"({"choices":[]})")), QString());
        QCOMPARE(acc.addChunk(json(R"({"usage":{"total_tokens":3}})")), QString());
        QVERIFY(acc.text().isEmpty());
        QVERIFY(!acc.hasError());
    }

    void testAccumulatesToolCallFragments()
    {
        CompletionAccumulator acc;
        acc.addChunk(toolCallChunk(0, "call_1", "paint", ""));
        acc.addChunk(toolCallChunk(0, "", "", "{\"col"));
        acc.addChunk(toolCallChunk(0, "", "", "or\":\"red\"}"));
        acc.addChunk(toolCallChunk(1, "call_2", "undo", "{}"));
        acc.addChunk(json(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"));

        const QList<ToolCall> calls = acc.toolCalls();
        QCOMPARE(calls.size(), 2);
        QCOMPARE(calls[0].id, QString("call_1"));
        QCOMPARE(calls[0].name, QString("paint"));
        QCOMPARE(calls[0].arguments, QString("{\"color\":\"red\"}"));
        QCOMPARE(calls[1].id, QString("call_2"));
        QCOMPARE(calls[1].name, QString("undo"));
        QCOMPARE(calls[1].arguments, QString("{}"));
        QCOMPARE(acc.finishReason(), QString("tool_calls"));
    }

    void testErrorChunk()
    {
        CompletionAccumulator acc;
        acc.addChunk(json(R"({"error":{"message":"Rate limit reached","type":"rate_limit"}})"));
        QVERIFY(acc.hasError());
        QCOMPARE(acc.error(), QString("Rate limit reached"));
    }

    void testErrorAsPlainString()
    {
        CompletionAccumulator acc;
        acc.addChunk(json(R"({"error":"boom"})"));
        QVERIFY(acc.hasError());
        QCOMPARE(acc.error(), QString("boom"));
    }

    void testFullResponse()
    {
        CompletionAccumulator acc;
        const QString text = acc.setFullResponse(json(
            R"({"choices":[{"message":{"role":"assistant","content":"All done."},"finish_reason":"stop"}]})"));
        QCOMPARE(text, QString("All done."));
        QCOMPARE(acc.text(), QString("All done."));
        QCOMPARE(acc.finishReason(), QString("stop"));
    }

    void testUsageOfAStreamedAnswer()
    {
        CompletionAccumulator acc;
        QCOMPARE(acc.totalTokens(), qint64(-1)); // not said yet

        acc.addChunk(json(R"({"choices":[{"delta":{"content":"Hi"}}]})"));
        QCOMPARE(acc.totalTokens(), qint64(-1));

        // the usage comes in a chunk of its own, without choices
        acc.addChunk(json(R"({"choices":[],"usage":{"prompt_tokens":40,"completion_tokens":2,"total_tokens":42}})"));
        QCOMPARE(acc.totalTokens(), qint64(42));
        QCOMPARE(acc.text(), QString("Hi"));
    }

    void testUsageInTheChunkWithTheText()
    {
        CompletionAccumulator acc;
        acc.addChunk(json(R"({"choices":[{"delta":{"content":"Hi"},"finish_reason":"stop"}],"usage":{"total_tokens":7}})"));
        QCOMPARE(acc.totalTokens(), qint64(7));
        QCOMPARE(acc.text(), QString("Hi"));
    }

    void testUsageWithoutATotalIsAddedUp()
    {
        CompletionAccumulator acc;
        acc.addChunk(json(R"({"choices":[],"usage":{"prompt_tokens":10,"completion_tokens":5}})"));
        QCOMPARE(acc.totalTokens(), qint64(15));
    }

    void testUsageOfAFullResponse()
    {
        CompletionAccumulator acc;
        acc.setFullResponse(json(
            R"({"choices":[{"message":{"content":"x"},"finish_reason":"stop"}],"usage":{"total_tokens":123}})"));
        QCOMPARE(acc.totalTokens(), qint64(123));
    }

    void testUsageThatIsNotAnObjectIsIgnored()
    {
        CompletionAccumulator acc;
        acc.addChunk(json(R"({"choices":[],"usage":null})"));
        acc.addChunk(json(R"({"choices":[],"usage":"lots"})"));
        QCOMPARE(acc.totalTokens(), qint64(-1));
    }

    void testFullResponseWithToolCalls()
    {
        CompletionAccumulator acc;
        acc.setFullResponse(fullToolCallResponse("c1", "paint", "{\"a\":1}"));
        QVERIFY(acc.text().isEmpty());
        QCOMPARE(acc.toolCalls().size(), 1);
        QCOMPARE(acc.toolCalls().first().name, QString("paint"));
        QCOMPARE(acc.toolCalls().first().arguments, QString("{\"a\":1}"));
    }

    void testFullResponseWithContentParts()
    {
        CompletionAccumulator acc;
        acc.setFullResponse(json(
            R"({"choices":[{"message":{"content":[{"type":"text","text":"Hello "},{"type":"text","text":"world"}]}}]})"));
        QCOMPARE(acc.text(), QString("Hello world"));
    }

    void testFullResponseError()
    {
        CompletionAccumulator acc;
        acc.setFullResponse(json(R"({"error":{"message":"Invalid API key"}})"));
        QVERIFY(acc.hasError());
        QCOMPARE(acc.error(), QString("Invalid API key"));
    }

    // ---- HTTP error text ---------------------------------------------

    void testHttpErrorMessage()
    {
        QCOMPARE(httpErrorMessage(401, R"({"error":{"message":"Incorrect API key provided"}})"),
                 QString("HTTP 401: Incorrect API key provided"));
        QCOMPARE(httpErrorMessage(502, "Bad Gateway"), QString("HTTP 502: Bad Gateway"));
        QCOMPARE(httpErrorMessage(500, ""), QString("HTTP 500"));
        // a huge HTML error page must not flood the chat
        const QString huge = httpErrorMessage(500, QByteArray(5000, 'x'));
        QVERIFY(huge.size() < 400);
    }
};

QTEST_MAIN(StreamTest)
#include "StreamTest.moc"

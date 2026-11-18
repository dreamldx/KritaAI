/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <memory>

#include "../AiChatAgent.h"
#include "../AiChatDocumentTools.h"
#include "../AiChatTools.h"
#include "../AiChatVision.h"
#include "FakeOpenAiServer.h"

namespace {

/// Runs `function` in a thread of its own while this thread keeps handling events (the GUI thread
/// has to, to serve the executor), and returns what it returned.
template<typename Function>
auto inWorkerThread(Function function) -> decltype(function())
{
    decltype(function()) result {};
    std::unique_ptr<QThread> thread(QThread::create([&]() { result = function(); }));
    thread->start();
    QElapsedTimer timer;
    timer.start();
    while (!thread->isFinished() && timer.elapsed() < 20000) {
        QTest::qWait(5);
    }
    thread->wait(5000);
    return result;
}

struct ToolResult {
    QString text;
    bool ok {false};
};

ToolResult runTool(AiChatToolRegistry &registry, const QString &name, const QString &arguments)
{
    ToolResult result;
    result.text = registry.runTool(name, arguments, &result.ok);
    return result;
}

/// A document that is not Krita's.
class FakeAccess : public AiChatDocumentAccess
{
public:
    bool documentInfo(QJsonObject *info, QString *error) override
    {
        calledInGuiThread = QThread::currentThread() == qApp->thread();
        if (!hasDocument) {
            *error = "There is no open document.";
            return false;
        }
        QJsonObject result;
        result["width"] = 200;
        result["height"] = 100;
        *info = result;
        return true;
    }

    bool renderImage(const QRect &region, bool selectionOnly, QImage *image, QRect *usedRegion, QString *error) override
    {
        calledInGuiThread = QThread::currentThread() == qApp->thread();
        lastRegion = region;
        lastSelectionOnly = selectionOnly;
        if (!hasDocument) {
            *error = "There is no open document.";
            return false;
        }
        QImage picture(200, 100, QImage::Format_ARGB32);
        picture.fill(QColor(255, 0, 0, 128)); // half transparent red
        *image = picture;
        *usedRegion = QRect(0, 0, 200, 100);
        return true;
    }

    bool hasDocument {true};
    bool calledInGuiThread {false};
    QRect lastRegion;
    bool lastSelectionOnly {false};
};

ScriptedResponse jsonReply(const QString &answer)
{
    QJsonObject message;
    message["role"] = "assistant";
    message["content"] = answer;
    QJsonObject choice;
    choice["message"] = message;
    QJsonObject body;
    body["choices"] = QJsonArray {choice};

    ScriptedResponse response;
    response.contentType = "application/json";
    response.chunks = {QJsonDocument(body).toJson(QJsonDocument::Compact)};
    return response;
}

AiChatRequestSettings settingsWithVision(const QUrl &url)
{
    AiChatRequestSettings settings;
    settings.vision.url = url;
    settings.vision.apiKey = "vision-key";
    settings.vision.model = "vision-model";
    return settings;
}

} // namespace

class AiChatToolsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ---- the registry ----

    void testDefinitionsUseTheOpenAiFormat()
    {
        AiChatToolRegistry registry;
        AiChatTool tool;
        tool.name = "hello";
        tool.description = "Says hello.";
        tool.run = [](const QJsonObject &, const AiChatRequestSettings &, bool *) { return QString("hi"); };
        registry.add(tool);

        const QJsonArray definitions = registry.toolDefinitions();
        QCOMPARE(definitions.size(), 1);
        const QJsonObject entry = definitions.first().toObject();
        QCOMPARE(entry["type"].toString(), QString("function"));
        const QJsonObject function = entry["function"].toObject();
        QCOMPARE(function["name"].toString(), QString("hello"));
        QCOMPARE(function["description"].toString(), QString("Says hello."));
        // a tool without arguments still has a schema
        QCOMPARE(function["parameters"].toObject()["type"].toString(), QString("object"));
    }

    void testAddingTheSameNameReplaces()
    {
        AiChatToolRegistry registry;
        AiChatTool tool;
        tool.name = "t";
        tool.run = [](const QJsonObject &, const AiChatRequestSettings &, bool *) { return QString("one"); };
        registry.add(tool);
        tool.run = [](const QJsonObject &, const AiChatRequestSettings &, bool *) { return QString("two"); };
        registry.add(tool);

        QCOMPARE(registry.names(), QStringList {"t"});
        QCOMPARE(runTool(registry, "t", "{}").text, QString("two"));
    }

    void testArgumentsReachTheTool()
    {
        AiChatToolRegistry registry;
        AiChatTool tool;
        tool.name = "echo";
        tool.run = [](const QJsonObject &arguments, const AiChatRequestSettings &, bool *) {
            return arguments.value("word").toString();
        };
        registry.add(tool);

        const ToolResult result = runTool(registry, "echo", "{\"word\":\"pear\"}");
        QVERIFY(result.ok);
        QCOMPARE(result.text, QString("pear"));
    }

    void testEmptyArgumentsAreAnEmptyObject()
    {
        AiChatToolRegistry registry;
        AiChatTool tool;
        tool.name = "count";
        tool.run = [](const QJsonObject &arguments, const AiChatRequestSettings &, bool *) {
            return QString::number(arguments.size());
        };
        registry.add(tool);

        QCOMPARE(runTool(registry, "count", "").text, QString("0"));
        QCOMPARE(runTool(registry, "count", "  ").text, QString("0"));
        QCOMPARE(runTool(registry, "count", "{}").text, QString("0"));
    }

    void testBadCallsAreExplainedToTheModel()
    {
        AiChatToolRegistry registry;
        AiChatTool tool;
        tool.name = "known";
        tool.run = [](const QJsonObject &, const AiChatRequestSettings &, bool *) { return QString("x"); };
        registry.add(tool);

        const ToolResult unknown = runTool(registry, "nope", "{}");
        QVERIFY(!unknown.ok);
        QVERIFY(unknown.text.contains("nope"));
        QVERIFY(unknown.text.contains("known")); // tells what exists

        const ToolResult broken = runTool(registry, "known", "{not json");
        QVERIFY(!broken.ok);
        QVERIFY(broken.text.contains("known"));

        const ToolResult notAnObject = runTool(registry, "known", "[1,2]");
        QVERIFY(!notAnObject.ok);
    }

    void testAToolCanFail()
    {
        AiChatToolRegistry registry;
        AiChatTool tool;
        tool.name = "fails";
        tool.run = [](const QJsonObject &, const AiChatRequestSettings &, bool *ok) {
            *ok = false;
            return QString("because");
        };
        registry.add(tool);

        const ToolResult result = runTool(registry, "fails", "{}");
        QVERIFY(!result.ok);
        QCOMPARE(result.text, QString("because"));
    }

    void testAToolCanDependOnTheSettingsOfTheRun()
    {
        AiChatToolRegistry registry;
        AiChatTool tool;
        tool.name = "needs_vision";
        tool.isAvailable = [](const AiChatRequestSettings &settings) { return settings.vision.isValid(); };
        tool.run = [](const QJsonObject &, const AiChatRequestSettings &, bool *) { return QString("seen"); };
        registry.add(tool);

        registry.beginRun(AiChatRequestSettings());
        QVERIFY(registry.toolDefinitions().isEmpty());
        QVERIFY(!runTool(registry, "needs_vision", "{}").ok); // it cannot be called either

        registry.beginRun(settingsWithVision(QUrl("http://localhost/v1/chat/completions")));
        QCOMPARE(registry.toolDefinitions().size(), 1);
        QVERIFY(runTool(registry, "needs_vision", "{}").ok);
    }

    // ---- the executor ----

    void testExecutorRunsInTheGuiThread()
    {
        AiChatGuiExecutor executor;
        bool ranInGuiThread = false;

        const bool ok = inWorkerThread([&]() {
            QString error;
            return executor.run([&]() { ranInGuiThread = QThread::currentThread() == qApp->thread(); }, &error);
        });
        QVERIFY(ok);
        QVERIFY(ranInGuiThread);
    }

    void testExecutorInTheGuiThreadJustCalls()
    {
        AiChatGuiExecutor executor;
        bool ran = false;
        QString error;
        QVERIFY(executor.run([&]() { ran = true; }, &error));
        QVERIFY(ran);
    }

    void testClosedExecutorReleasesAWaitingThread()
    {
        AiChatGuiExecutor executor;
        bool ran = false;
        QString error;
        bool result = true;

        // The GUI thread does not handle events (it is busy), so the function cannot start.
        std::unique_ptr<QThread> thread(QThread::create([&]() {
            result = executor.run([&]() { ran = true; }, &error, 20000);
        }));
        thread->start();
        QThread::msleep(150);
        executor.close(); // before it, the thread would wait for the full 20 seconds
        QVERIFY(thread->wait(5000));

        QVERIFY(!result);
        QVERIFY(!error.isEmpty());

        // the function that was still waiting in the event queue must not run any more
        QTest::qWait(50);
        QVERIFY(!ran);

        // and a closed executor refuses at once
        bool again = true;
        std::unique_ptr<QThread> second(QThread::create([&]() { again = executor.run([&]() { ran = true; }, &error, 20000); }));
        second->start();
        QVERIFY(second->wait(2000));
        QVERIFY(!again);
        QVERIFY(executor.isClosed());
    }

    void testExecutorGivesUpWhenTheGuiThreadStaysBusy()
    {
        AiChatGuiExecutor executor;
        QString error;
        bool result = true;

        std::unique_ptr<QThread> thread(QThread::create([&]() {
            result = executor.run([]() {}, &error, 200);
        }));
        thread->start();
        QVERIFY(thread->wait(5000)); // this thread never handles events meanwhile
        QVERIFY(!result);
        QVERIFY(error.contains("respond"));
        QTest::qWait(20);
    }

    // ---- get_document_info ----

    void testDocumentInfoTool()
    {
        AiChatGuiExecutor executor;
        FakeAccess access;
        AiChatVisionClient vision;
        AiChatToolRegistry registry;
        aiChatAddDocumentTools(&registry, &executor, &access, &vision);

        const ToolResult result = inWorkerThread([&]() { return runTool(registry, "get_document_info", "{}"); });
        QVERIFY(result.ok);
        QVERIFY(access.calledInGuiThread);
        const QJsonObject info = QJsonDocument::fromJson(result.text.toUtf8()).object();
        QCOMPARE(info["width"].toInt(), 200);
        QCOMPARE(info["height"].toInt(), 100);
    }

    void testDocumentInfoWithoutDocument()
    {
        AiChatGuiExecutor executor;
        FakeAccess access;
        access.hasDocument = false;
        AiChatVisionClient vision;
        AiChatToolRegistry registry;
        aiChatAddDocumentTools(&registry, &executor, &access, &vision);

        const ToolResult result = inWorkerThread([&]() { return runTool(registry, "get_document_info", "{}"); });
        QVERIFY(!result.ok);
        QVERIFY(result.text.contains("no open document"));
    }

    // ---- ask_about_image ----

    void testAskAboutImageIsOnlyOfferedWithAVisionModel()
    {
        AiChatGuiExecutor executor;
        FakeAccess access;
        AiChatVisionClient vision;
        AiChatToolRegistry registry;
        aiChatAddDocumentTools(&registry, &executor, &access, &vision);

        registry.beginRun(AiChatRequestSettings());
        QStringList without;
        for (const QJsonValue &entry : registry.toolDefinitions()) {
            without << entry.toObject()["function"].toObject()["name"].toString();
        }
        QCOMPARE(without, QStringList {"get_document_info"});

        registry.beginRun(settingsWithVision(QUrl("http://localhost/v1/chat/completions")));
        QStringList with;
        for (const QJsonValue &entry : registry.toolDefinitions()) {
            with << entry.toObject()["function"].toObject()["name"].toString();
        }
        QVERIFY(with.contains("ask_about_image"));
        QVERIFY(with.contains("get_document_info"));
    }

    void testAskAboutImageSendsThePictureAndReturnsTheAnswer()
    {
        FakeServer server;
        QVERIFY(server.start());
        server.enqueue(jsonReply("A red rectangle."));

        AiChatGuiExecutor executor;
        FakeAccess access;
        AiChatVisionClient vision;
        AiChatToolRegistry registry;
        aiChatAddDocumentTools(&registry, &executor, &access, &vision);
        registry.beginRun(settingsWithVision(server.chatUrl()));

        const ToolResult result = inWorkerThread([&]() {
            return runTool(registry, "ask_about_image",
                           "{\"question\":\"What color is it?\",\"region\":{\"x\":0,\"y\":0,\"width\":50,\"height\":40}}");
        });

        QVERIFY2(result.ok, qPrintable(result.text));
        QVERIFY(result.text.contains("A red rectangle."));
        QVERIFY(access.calledInGuiThread);
        QCOMPARE(access.lastRegion, QRect(0, 0, 50, 40));
        QVERIFY(!access.lastSelectionOnly);

        QCOMPARE(server.requests.size(), 1);
        const RecordedRequest request = server.requests.first();
        QCOMPARE(request.headers.value("authorization"), QByteArray("Bearer vision-key"));
        const QJsonObject body = request.json();
        QCOMPARE(body["model"].toString(), QString("vision-model"));
        QCOMPARE(body["stream"].toBool(true), false);
        // looking at a picture does not need the model to think: that only costs time and tokens
        QCOMPARE(body["reasoning"].toObject()["enabled"].toBool(true), false);

        const QJsonArray content = request.messages().first().toObject()["content"].toArray();
        QCOMPARE(content.size(), 2);
        QCOMPARE(content[0].toObject()["type"].toString(), QString("text"));
        QCOMPARE(content[0].toObject()["text"].toString(), QString("What color is it?"));
        QCOMPARE(content[1].toObject()["type"].toString(), QString("image_url"));
        const QString uri = content[1].toObject()["image_url"].toObject()["url"].toString();
        QVERIFY(uri.startsWith("data:image/png;base64,"));

        // what was sent really is the picture: a PNG that decodes, and the transparent
        // half-red does not turn black but shows on white
        const QImage sent = QImage::fromData(QByteArray::fromBase64(uri.mid(22).toLatin1()), "PNG");
        QVERIFY(!sent.isNull());
        QCOMPARE(sent.size(), QSize(200, 100));
        const QColor pixel = sent.pixelColor(10, 10);
        QVERIFY(pixel.red() > 200);
        QVERIFY(pixel.green() > 100); // white shines through
    }

    void testThinkingIsOnlySwitchedOffWhenAsked()
    {
        const QByteArray off = AiChatVisionClient::buildRequestBody("m", "?", "x", "image/png", true);
        QVERIFY(QJsonDocument::fromJson(off).object().contains("reasoning"));

        const QByteArray left = AiChatVisionClient::buildRequestBody("m", "?", "x", "image/png", false);
        QVERIFY(!QJsonDocument::fromJson(left).object().contains("reasoning")); // servers that reject the field

        // the default of the settings is to switch it off
        QVERIFY(AiChatVisionSettings().disableThinking);
    }

    void testBigPicturesAreShrunk()
    {
        class BigAccess : public FakeAccess
        {
        public:
            bool renderImage(const QRect &, bool, QImage *image, QRect *used, QString *) override
            {
                QImage picture(4000, 2000, QImage::Format_ARGB32);
                picture.fill(Qt::blue);
                *image = picture;
                *used = QRect(0, 0, 4000, 2000);
                return true;
            }
        };

        FakeServer server;
        QVERIFY(server.start());
        server.enqueue(jsonReply("Blue."));

        AiChatGuiExecutor executor;
        BigAccess access;
        AiChatVisionClient vision;
        AiChatToolRegistry registry;
        aiChatAddDocumentTools(&registry, &executor, &access, &vision);
        registry.beginRun(settingsWithVision(server.chatUrl()));

        const ToolResult result = inWorkerThread([&]() { return runTool(registry, "ask_about_image", "{\"question\":\"?\"}"); });
        QVERIFY2(result.ok, qPrintable(result.text));

        const QString uri = server.requests.first().messages().first().toObject()["content"].toArray()[1]
                                .toObject()["image_url"].toObject()["url"].toString();
        const QImage sent = QImage::fromData(QByteArray::fromBase64(uri.mid(22).toLatin1()), "PNG");
        QCOMPARE(sent.size(), QSize(kAiChatVisionMaxSide, kAiChatVisionMaxSide / 2));
    }

    void testSelectionOnlyIsPassedOn()
    {
        FakeServer server;
        QVERIFY(server.start());
        server.enqueue(jsonReply("ok"));

        AiChatGuiExecutor executor;
        FakeAccess access;
        AiChatVisionClient vision;
        AiChatToolRegistry registry;
        aiChatAddDocumentTools(&registry, &executor, &access, &vision);
        registry.beginRun(settingsWithVision(server.chatUrl()));

        const ToolResult result = inWorkerThread([&]() {
            return runTool(registry, "ask_about_image", "{\"question\":\"?\",\"selection_only\":true}");
        });
        QVERIFY(result.ok);
        QVERIFY(access.lastSelectionOnly);
    }

    void testBadArgumentsOfAskAboutImage()
    {
        AiChatGuiExecutor executor;
        FakeAccess access;
        AiChatVisionClient vision;
        AiChatToolRegistry registry;
        aiChatAddDocumentTools(&registry, &executor, &access, &vision);
        registry.beginRun(settingsWithVision(QUrl("http://127.0.0.1:1/v1/chat/completions")));

        QVERIFY(!runTool(registry, "ask_about_image", "{}").ok); // no question
        QVERIFY(!runTool(registry, "ask_about_image", "{\"question\":\"  \"}").ok);
        QVERIFY(!runTool(registry, "ask_about_image", "{\"question\":\"?\",\"region\":{\"x\":1}}").ok);
        QVERIFY(!runTool(registry, "ask_about_image", "{\"question\":\"?\",\"region\":5}").ok);
        const ToolResult empty = runTool(registry, "ask_about_image",
                                         "{\"question\":\"?\",\"region\":{\"x\":0,\"y\":0,\"width\":0,\"height\":10}}");
        QVERIFY(!empty.ok);
    }

    void testNoDocumentForAskAboutImage()
    {
        AiChatGuiExecutor executor;
        FakeAccess access;
        access.hasDocument = false;
        AiChatVisionClient vision;
        AiChatToolRegistry registry;
        aiChatAddDocumentTools(&registry, &executor, &access, &vision);
        registry.beginRun(settingsWithVision(QUrl("http://127.0.0.1:1/v1/chat/completions")));

        const ToolResult result = inWorkerThread([&]() { return runTool(registry, "ask_about_image", "{\"question\":\"?\"}"); });
        QVERIFY(!result.ok);
        QVERIFY(result.text.contains("no open document"));
    }

    void testVisionServerError()
    {
        FakeServer server;
        QVERIFY(server.start());
        ScriptedResponse unauthorized;
        unauthorized.status = 401;
        unauthorized.contentType = "application/json";
        unauthorized.chunks = {"{\"error\":{\"message\":\"Bad vision key\"}}"};
        server.enqueue(unauthorized);

        AiChatGuiExecutor executor;
        FakeAccess access;
        AiChatVisionClient vision;
        AiChatToolRegistry registry;
        aiChatAddDocumentTools(&registry, &executor, &access, &vision);
        registry.beginRun(settingsWithVision(server.chatUrl()));

        const ToolResult result = inWorkerThread([&]() { return runTool(registry, "ask_about_image", "{\"question\":\"?\"}"); });
        QVERIFY(!result.ok);
        QVERIFY(result.text.contains("Bad vision key"));
        QVERIFY(result.text.contains("401"));
    }

    void testVisionAnswerWithoutText()
    {
        QString error;
        QVERIFY(AiChatVisionClient::parseAnswer("not json", &error).isEmpty());
        QVERIFY(!error.isEmpty());

        error.clear();
        QVERIFY(AiChatVisionClient::parseAnswer("{\"choices\":[]}", &error).isEmpty());
        QVERIFY(!error.isEmpty());

        error.clear();
        QVERIFY(AiChatVisionClient::parseAnswer("{\"choices\":[{\"message\":{\"content\":\"  \"}}]}", &error).isEmpty());
        QVERIFY(!error.isEmpty());

        // a list of parts instead of a string
        error.clear();
        QCOMPARE(AiChatVisionClient::parseAnswer("{\"choices\":[{\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"A \"},"
                                                 "{\"type\":\"text\",\"text\":\"cat\"}]}}]}", &error),
                 QString("A cat"));
    }

    void testCancelStopsAWaitingVisionRequest()
    {
        FakeServer server;
        QVERIFY(server.start());
        ScriptedResponse hanging;
        hanging.contentType = "application/json";
        hanging.firstDelayMs = 60000; // never answers in this test
        hanging.chunks = {"{}"};
        server.enqueue(hanging);

        AiChatVisionClient vision;
        QString error;
        QString answer = "unset";
        std::unique_ptr<QThread> thread(QThread::create([&]() {
            AiChatVisionSettings settings;
            settings.url = server.chatUrl();
            settings.model = "m";
            answer = vision.ask(settings, "?", "x", "image/png", &error, 60000);
        }));
        thread->start();
        QTest::qWait(300);
        QElapsedTimer timer;
        timer.start();
        vision.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(thread->isFinished(), 5000);
        thread->wait();

        QVERIFY(answer.isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(timer.elapsed() < 5000);
    }

    void testVisionTimeout()
    {
        FakeServer server;
        QVERIFY(server.start());
        ScriptedResponse hanging;
        hanging.contentType = "application/json";
        hanging.firstDelayMs = 60000;
        hanging.chunks = {"{}"};
        server.enqueue(hanging);

        AiChatVisionClient vision;
        QString error;
        QString answer;
        std::unique_ptr<QThread> thread(QThread::create([&]() {
            AiChatVisionSettings settings;
            settings.url = server.chatUrl();
            settings.model = "m";
            answer = vision.ask(settings, "?", "x", "image/png", &error, 300);
        }));
        thread->start();
        QTRY_VERIFY_WITH_TIMEOUT(thread->isFinished(), 5000);
        thread->wait();
        QVERIFY(answer.isEmpty());
        QVERIFY(error.contains("in time"));
    }

    // ---- the AI loop with the tools ----

    void testTheModelUsesATool()
    {
        FakeServer server;
        QVERIFY(server.start());

        // 1st answer: the model asks for the document info; 2nd: it answers with text
        ScriptedResponse call;
        call.chunks = {
            toolCallChunk(0, "call_info", "get_document_info", "{}"),
            finishChunk("tool_calls"),
            doneEvent(),
        };
        server.enqueue(call);
        server.enqueue(textReply("The image is 200 x 100."));

        AiChatGuiExecutor executor;
        FakeAccess access;
        AiChatVisionClient vision;
        AiChatToolRegistry registry;
        aiChatAddDocumentTools(&registry, &executor, &access, &vision);

        AiChatAgent agent;
        agent.setToolProvider(&registry);
        AiChatRequestSettings settings;
        settings.url = server.chatUrl();
        settings.model = "chat-model";
        QSignalSpy finished(&agent, &AiChatAgent::finished);

        QJsonObject user;
        user["role"] = "user";
        user["content"] = "How big is my image?";
        agent.start(settings, QJsonArray {user});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(finished.first().first().toString(), QString("The image is 200 x 100."));

        // the first request offered the tool; vision is not set up, so ask_about_image was not offered
        const QJsonArray tools = server.requests[0].json()["tools"].toArray();
        QCOMPARE(tools.size(), 1);
        QCOMPARE(tools[0].toObject()["function"].toObject()["name"].toString(), QString("get_document_info"));

        // the second request carried the result of the tool back to the model
        const QJsonArray messages = server.requests[1].messages();
        const QJsonObject toolMessage = messages.last().toObject();
        QCOMPARE(toolMessage["role"].toString(), QString("tool"));
        QCOMPARE(toolMessage["tool_call_id"].toString(), QString("call_info"));
        QVERIFY(toolMessage["content"].toString().contains("\"width\":200"));
    }

    void testCancelingTheLoopCancelsTheTool()
    {
        // the registry's cancel hook is what returns a waiting tool
        AiChatToolRegistry registry;
        bool hookCalled = false;
        registry.setCancelHook([&]() { hookCalled = true; });
        registry.cancel();
        QVERIFY(hookCalled);
    }
};

QTEST_MAIN(AiChatToolsTest)

#include "AiChatToolsTest.moc"

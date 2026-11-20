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

#include "../ai/Agent.h"
#include "../ai/tools/DocumentTools.h"
#include "../ai/tools/Fal.h"
#include "../ai/tools/FalTools.h"
#include "../ai/tools/LayerTools.h"
#include "../ui/Progress.h"
#include "../ai/tools/Tools.h"
#include "../ai/tools/Vision.h"
#include "FakeOpenAiServer.h"

using namespace AiChat;

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

ToolResult runTool(ToolRegistry &registry, const QString &name, const QString &arguments)
{
    ToolResult result;
    result.text = registry.runTool(name, arguments, &result.ok);
    return result;
}

/// A document that is not Krita's.
class FakeAccess : public DocumentAccess
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
        QImage picture(renderedRegion.size(), QImage::Format_ARGB32);
        picture.fill(QColor(255, 0, 0, 128)); // half transparent red
        *image = picture;
        *usedRegion = renderedRegion;
        return true;
    }

    bool applyLayerCommand(const LayerCommand &command, QString *summary, QString *error) override
    {
        calledInGuiThread = QThread::currentThread() == qApp->thread();
        if (!canApply) {
            *error = "no window";
            return false;
        }
        commands.append(command);
        *summary = "Did it.";
        return true;
    }

    bool hasDocument {true};
    bool canApply {true};
    QRect renderedRegion {0, 0, 200, 100}; ///< what renderImage() gives, whatever is asked
    QList<LayerCommand> commands; ///< the layer changes that were asked for
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

RequestSettings settingsWithVision(const QUrl &url)
{
    RequestSettings settings;
    settings.vision.url = url;
    settings.vision.apiKey = "vision-key";
    settings.vision.model = "vision-model";
    return settings;
}

// ---- fal.ai (the Topaz tools) ----

ScriptedResponse falJson(const QJsonObject &object)
{
    ScriptedResponse response;
    response.contentType = "application/json";
    response.chunks = {QJsonDocument(object).toJson(QJsonDocument::Compact)};
    return response;
}

QJsonObject falSubmitted(const FakeServer &server, const QString &app)
{
    QJsonObject object;
    object["request_id"] = "req-1";
    object["status_url"] = QString(server.baseUrl().toString() + "/topaz/" + app + "/requests/req-1/status");
    object["response_url"] = QString(server.baseUrl().toString() + "/topaz/" + app + "/requests/req-1");
    return object;
}

/// The two steps of putting the picture into the file storage: a place is given, the bytes are put there.
void scriptFalUpload(FakeServer &server)
{
    QJsonObject place;
    place["upload_url"] = QString(server.baseUrl().toString() + "/upload/abc");
    place["file_url"] = QString(server.baseUrl().toString() + "/files/in.png");
    server.enqueue(falJson(place));
    ScriptedResponse stored;
    stored.contentType = "text/plain";
    stored.chunks = {"ok"};
    server.enqueue(stored);
}

/// Upload, accepted, working, done, the result, the picture (40 x 20 pixels).
void scriptFalJob(FakeServer &server, const QString &app = "upscale")
{
    scriptFalUpload(server);
    server.enqueue(falJson(falSubmitted(server, app)));
    QJsonObject working;
    working["status"] = "IN_PROGRESS";
    server.enqueue(falJson(working));
    QJsonObject done;
    done["status"] = "COMPLETED";
    server.enqueue(falJson(done));

    QJsonObject image;
    image["url"] = QString(server.baseUrl().toString() + "/files/result.png");
    QJsonObject result;
    result["image"] = image;
    server.enqueue(falJson(result));

    QImage picture(40, 20, QImage::Format_ARGB32);
    picture.fill(Qt::green);
    ScriptedResponse file;
    file.contentType = "image/png";
    file.chunks = {VisionClient::encodePng(picture)};
    server.enqueue(file);

    // and the uploaded picture is deleted again
    ScriptedResponse deleted;
    deleted.contentType = "application/json";
    deleted.chunks = {"{}"};
    server.enqueue(deleted);
}

RequestSettings settingsWithFal(const FakeServer &server)
{
    RequestSettings settings;
    settings.fal.apiKey = "fal-secret";
    settings.fal.queueUrl = server.baseUrl();
    settings.fal.restUrl = server.baseUrl();
    settings.fal.deleteUrl = server.baseUrl();
    return settings;
}

} // namespace

class ToolsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ---- the registry ----

    void testDefinitionsUseTheOpenAiFormat()
    {
        ToolRegistry registry;
        Tool tool;
        tool.name = "hello";
        tool.description = "Says hello.";
        tool.run = [](const QJsonObject &, const RequestSettings &, bool *) { return QString("hi"); };
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
        ToolRegistry registry;
        Tool tool;
        tool.name = "t";
        tool.run = [](const QJsonObject &, const RequestSettings &, bool *) { return QString("one"); };
        registry.add(tool);
        tool.run = [](const QJsonObject &, const RequestSettings &, bool *) { return QString("two"); };
        registry.add(tool);

        QCOMPARE(registry.names(), QStringList {"t"});
        QCOMPARE(runTool(registry, "t", "{}").text, QString("two"));
    }

    void testArgumentsReachTheTool()
    {
        ToolRegistry registry;
        Tool tool;
        tool.name = "echo";
        tool.run = [](const QJsonObject &arguments, const RequestSettings &, bool *) {
            return arguments.value("word").toString();
        };
        registry.add(tool);

        const ToolResult result = runTool(registry, "echo", "{\"word\":\"pear\"}");
        QVERIFY(result.ok);
        QCOMPARE(result.text, QString("pear"));
    }

    void testEmptyArgumentsAreAnEmptyObject()
    {
        ToolRegistry registry;
        Tool tool;
        tool.name = "count";
        tool.run = [](const QJsonObject &arguments, const RequestSettings &, bool *) {
            return QString::number(arguments.size());
        };
        registry.add(tool);

        QCOMPARE(runTool(registry, "count", "").text, QString("0"));
        QCOMPARE(runTool(registry, "count", "  ").text, QString("0"));
        QCOMPARE(runTool(registry, "count", "{}").text, QString("0"));
    }

    void testBadCallsAreExplainedToTheModel()
    {
        ToolRegistry registry;
        Tool tool;
        tool.name = "known";
        tool.run = [](const QJsonObject &, const RequestSettings &, bool *) { return QString("x"); };
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
        ToolRegistry registry;
        Tool tool;
        tool.name = "fails";
        tool.run = [](const QJsonObject &, const RequestSettings &, bool *ok) {
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
        ToolRegistry registry;
        Tool tool;
        tool.name = "needs_vision";
        tool.isAvailable = [](const RequestSettings &settings) { return settings.vision.isValid(); };
        tool.run = [](const QJsonObject &, const RequestSettings &, bool *) { return QString("seen"); };
        registry.add(tool);

        registry.beginRun(RequestSettings());
        QVERIFY(registry.toolDefinitions().isEmpty());
        QVERIFY(!runTool(registry, "needs_vision", "{}").ok); // it cannot be called either

        registry.beginRun(settingsWithVision(QUrl("http://localhost/v1/chat/completions")));
        QCOMPARE(registry.toolDefinitions().size(), 1);
        QVERIFY(runTool(registry, "needs_vision", "{}").ok);
    }

    // ---- the executor ----

    void testExecutorRunsInTheGuiThread()
    {
        GuiExecutor executor;
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
        GuiExecutor executor;
        bool ran = false;
        QString error;
        QVERIFY(executor.run([&]() { ran = true; }, &error));
        QVERIFY(ran);
    }

    void testClosedExecutorReleasesAWaitingThread()
    {
        GuiExecutor executor;
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
        GuiExecutor executor;
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
        GuiExecutor executor;
        FakeAccess access;
        VisionClient vision;
        ToolRegistry registry;
        addDocumentTools(&registry, &executor, &access, &vision);

        const ToolResult result = inWorkerThread([&]() { return runTool(registry, "get_document_info", "{}"); });
        QVERIFY(result.ok);
        QVERIFY(access.calledInGuiThread);
        const QJsonObject info = QJsonDocument::fromJson(result.text.toUtf8()).object();
        QCOMPARE(info["width"].toInt(), 200);
        QCOMPARE(info["height"].toInt(), 100);
    }

    void testDocumentInfoWithoutDocument()
    {
        GuiExecutor executor;
        FakeAccess access;
        access.hasDocument = false;
        VisionClient vision;
        ToolRegistry registry;
        addDocumentTools(&registry, &executor, &access, &vision);

        const ToolResult result = inWorkerThread([&]() { return runTool(registry, "get_document_info", "{}"); });
        QVERIFY(!result.ok);
        QVERIFY(result.text.contains("no open document"));
    }

    // ---- ask_about_image ----

    void testAskAboutImageIsOnlyOfferedWithAVisionModel()
    {
        GuiExecutor executor;
        FakeAccess access;
        VisionClient vision;
        ToolRegistry registry;
        addDocumentTools(&registry, &executor, &access, &vision);

        registry.beginRun(RequestSettings());
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

        GuiExecutor executor;
        FakeAccess access;
        VisionClient vision;
        ToolRegistry registry;
        addDocumentTools(&registry, &executor, &access, &vision);
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
        const QString sentQuestion = content[0].toObject()["text"].toString();
        QVERIFY(sentQuestion.startsWith("What color is it?"));
        // positions are asked for as fractions 0..1 of the picture, never pixels
        QVERIFY(sentQuestion.contains("[left, top, right, bottom]"));
        QVERIFY(sentQuestion.contains("0 to 1"));
        // the model that reads the answer is told what the numbers are relative to, and how to pass the area on
        QVERIFY(result.text.contains("fractions 0 to 1 of this area"));
        QVERIFY(result.text.contains("region={\"x\":0,\"y\":0,\"width\":200,\"height\":100}"));
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
        const QByteArray off = VisionClient::buildRequestBody("m", "?", "x", "image/png", true);
        QVERIFY(QJsonDocument::fromJson(off).object().contains("reasoning"));

        const QByteArray left = VisionClient::buildRequestBody("m", "?", "x", "image/png", false);
        QVERIFY(!QJsonDocument::fromJson(left).object().contains("reasoning")); // servers that reject the field

        // the default of the settings is to switch it off
        QVERIFY(VisionSettings().disableThinking);
    }

    void testBlockedProvidersAreSentAsIgnore()
    {
        // nothing blocked: the field is not there at all (other servers need not know it)
        const QByteArray none = VisionClient::buildRequestBody("m", "?", "x", "image/png", true);
        QVERIFY(!QJsonDocument::fromJson(none).object().contains("provider"));

        const QByteArray blocked = VisionClient::buildRequestBody("m", "?", "x", "image/png", true,
                                                                        QStringList {"DeepInfra", "Sail Research"});
        const QJsonObject provider = QJsonDocument::fromJson(blocked).object()["provider"].toObject();
        QCOMPARE(provider["ignore"].toArray(), (QJsonArray {"DeepInfra", "Sail Research"}));
        // it is independent of the thinking switch
        QVERIFY(QJsonDocument::fromJson(blocked).object().contains("reasoning"));
        const QByteArray blockedAndThinking = VisionClient::buildRequestBody("m", "?", "x", "image/png", false,
                                                                                   QStringList {"X"});
        QVERIFY(!QJsonDocument::fromJson(blockedAndThinking).object().contains("reasoning"));
        QVERIFY(QJsonDocument::fromJson(blockedAndThinking).object().contains("provider"));
    }

    void testBlockedProvidersReachTheServer()
    {
        FakeServer server;
        QVERIFY(server.start());
        server.enqueue(jsonReply("ok"));

        GuiExecutor executor;
        FakeAccess access;
        VisionClient vision;
        ToolRegistry registry;
        addDocumentTools(&registry, &executor, &access, &vision);
        RequestSettings settings = settingsWithVision(server.chatUrl());
        settings.vision.ignoreProviders = QStringList {"DeepInfra", "Together"};
        registry.beginRun(settings);

        const ToolResult result = inWorkerThread([&]() { return runTool(registry, "ask_about_image", "{\"question\":\"?\"}"); });
        QVERIFY2(result.ok, qPrintable(result.text));
        const QJsonObject provider = server.requests.first().json()["provider"].toObject();
        QCOMPARE(provider["ignore"].toArray(), (QJsonArray {"DeepInfra", "Together"}));
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

        GuiExecutor executor;
        BigAccess access;
        VisionClient vision;
        ToolRegistry registry;
        addDocumentTools(&registry, &executor, &access, &vision);
        registry.beginRun(settingsWithVision(server.chatUrl()));

        const ToolResult result = inWorkerThread([&]() { return runTool(registry, "ask_about_image", "{\"question\":\"?\"}"); });
        QVERIFY2(result.ok, qPrintable(result.text));

        const QString uri = server.requests.first().messages().first().toObject()["content"].toArray()[1]
                                .toObject()["image_url"].toObject()["url"].toString();
        const QImage sent = QImage::fromData(QByteArray::fromBase64(uri.mid(22).toLatin1()), "PNG");
        QCOMPARE(sent.size(), QSize(kVisionMaxSide, kVisionMaxSide / 2));
    }

    void testSelectionOnlyIsPassedOn()
    {
        FakeServer server;
        QVERIFY(server.start());
        server.enqueue(jsonReply("ok"));

        GuiExecutor executor;
        FakeAccess access;
        VisionClient vision;
        ToolRegistry registry;
        addDocumentTools(&registry, &executor, &access, &vision);
        registry.beginRun(settingsWithVision(server.chatUrl()));

        const ToolResult result = inWorkerThread([&]() {
            return runTool(registry, "ask_about_image", "{\"question\":\"?\",\"selection_only\":true}");
        });
        QVERIFY(result.ok);
        QVERIFY(access.lastSelectionOnly);
    }

    void testBadArgumentsOfAskAboutImage()
    {
        GuiExecutor executor;
        FakeAccess access;
        VisionClient vision;
        ToolRegistry registry;
        addDocumentTools(&registry, &executor, &access, &vision);
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
        GuiExecutor executor;
        FakeAccess access;
        access.hasDocument = false;
        VisionClient vision;
        ToolRegistry registry;
        addDocumentTools(&registry, &executor, &access, &vision);
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

        GuiExecutor executor;
        FakeAccess access;
        VisionClient vision;
        ToolRegistry registry;
        addDocumentTools(&registry, &executor, &access, &vision);
        registry.beginRun(settingsWithVision(server.chatUrl()));

        const ToolResult result = inWorkerThread([&]() { return runTool(registry, "ask_about_image", "{\"question\":\"?\"}"); });
        QVERIFY(!result.ok);
        QVERIFY(result.text.contains("Bad vision key"));
        QVERIFY(result.text.contains("401"));
    }

    void testVisionAnswerWithoutText()
    {
        QString error;
        QVERIFY(VisionClient::parseAnswer("not json", &error).isEmpty());
        QVERIFY(!error.isEmpty());

        error.clear();
        QVERIFY(VisionClient::parseAnswer("{\"choices\":[]}", &error).isEmpty());
        QVERIFY(!error.isEmpty());

        error.clear();
        QVERIFY(VisionClient::parseAnswer("{\"choices\":[{\"message\":{\"content\":\"  \"}}]}", &error).isEmpty());
        QVERIFY(!error.isEmpty());

        // a list of parts instead of a string
        error.clear();
        QCOMPARE(VisionClient::parseAnswer("{\"choices\":[{\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"A \"},"
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

        VisionClient vision;
        QString error;
        QString answer = "unset";
        std::unique_ptr<QThread> thread(QThread::create([&]() {
            VisionSettings settings;
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

        VisionClient vision;
        QString error;
        QString answer;
        std::unique_ptr<QThread> thread(QThread::create([&]() {
            VisionSettings settings;
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

        GuiExecutor executor;
        FakeAccess access;
        VisionClient vision;
        ToolRegistry registry;
        addDocumentTools(&registry, &executor, &access, &vision);

        Agent agent;
        agent.setToolProvider(&registry);
        RequestSettings settings;
        settings.url = server.chatUrl();
        settings.model = "chat-model";
        QSignalSpy finished(&agent, &Agent::finished);

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
        ToolRegistry registry;
        bool hookCalled = false;
        registry.setCancelHook([&]() { hookCalled = true; });
        registry.cancel();
        QVERIFY(hookCalled);

        // more than one tool may need one
        bool second = false;
        registry.addCancelHook([&]() { second = true; });
        hookCalled = false;
        registry.cancel();
        QVERIFY(hookCalled);
        QVERIFY(second);
    }

    // ---- the layer tools ----

    void testAllLayerToolsAreOffered()
    {
        GuiExecutor executor;
        FakeAccess access;
        ToolRegistry registry;
        addLayerTools(&registry, &executor, &access);
        registry.beginRun(RequestSettings());

        QStringList names;
        for (const QJsonValue &entry : registry.toolDefinitions()) {
            const QJsonObject function = entry.toObject()["function"].toObject();
            names << function["name"].toString();
            // the model is told that the user can undo it
            QVERIFY2(function["description"].toString().contains("Undo"), qPrintable(function["name"].toString()));
        }
        names.sort();
        QCOMPARE(names, (QStringList {"add_group", "add_layer", "box_select", "delete_layer", "duplicate_layer", "move_layer", "move_layer_to_group",
                                      "rename_layer", "reorder_layer", "scale_image", "set_layer_blend_mode", "set_layer_opacity",
                                      "set_layer_visibility"}));
    }

    void testLayerToolsMakeTheRightCommands()
    {
        GuiExecutor executor;
        FakeAccess access;
        ToolRegistry registry;
        addLayerTools(&registry, &executor, &access);
        registry.beginRun(RequestSettings());

        const auto call = [&](const QString &tool, const QString &arguments) {
            return inWorkerThread([&]() { return runTool(registry, tool, arguments); });
        };

        ToolResult result = call("add_layer", "{\"name\":\"Notes\",\"above\":\"Sky\"}");
        QVERIFY2(result.ok, qPrintable(result.text));
        QCOMPARE(result.text, QString("Did it.")); // what the document said, for the model to read
        QVERIFY(access.calledInGuiThread);
        QCOMPARE(int(access.commands.last().type), int(LayerCommand::AddLayer));
        QCOMPARE(access.commands.last().text, QString("Notes"));
        QCOMPARE(access.commands.last().layer, QString("Sky"));

        QVERIFY(call("add_group", "{}").ok);
        QCOMPARE(int(access.commands.last().type), int(LayerCommand::AddGroup));

        QVERIFY(call("rename_layer", "{\"layer\":\"A\",\"name\":\"B\"}").ok);
        QCOMPARE(int(access.commands.last().type), int(LayerCommand::Rename));
        QCOMPARE(access.commands.last().layer, QString("A"));
        QCOMPARE(access.commands.last().text, QString("B"));

        QVERIFY(call("set_layer_visibility", "{\"layer\":\"A\",\"visible\":false}").ok);
        QCOMPARE(int(access.commands.last().type), int(LayerCommand::SetVisible));
        QVERIFY(!access.commands.last().flag);

        QVERIFY(call("set_layer_opacity", "{\"layer\":\"A\",\"opacity_percent\":35.5}").ok);
        QCOMPARE(access.commands.last().number, 35.5);

        QVERIFY(call("set_layer_blend_mode", "{\"layer\":\"A\",\"mode\":\"multiply\"}").ok);
        QCOMPARE(access.commands.last().text, QString("multiply"));

        QVERIFY(call("move_layer", "{\"layer\":\"A\",\"dx\":-5,\"dy\":12}").ok);
        QCOMPARE(access.commands.last().point, QPoint(-5, 12));

        QVERIFY(call("duplicate_layer", "{\"layer\":\"A\"}").ok);
        QCOMPARE(int(access.commands.last().type), int(LayerCommand::Duplicate));
        QVERIFY(call("delete_layer", "{\"layer\":\"A\"}").ok);
        QCOMPARE(int(access.commands.last().type), int(LayerCommand::Delete));

        QVERIFY(call("reorder_layer", "{\"layer\":\"A\",\"to\":\"top\"}").ok);
        QCOMPARE(access.commands.last().text, QString("top"));

        QVERIFY(call("move_layer_to_group", "{\"layer\":\"A\",\"group\":\"G\"}").ok);
        QCOMPARE(access.commands.last().text, QString("G"));
        QVERIFY(call("move_layer_to_group", "{\"layer\":\"A\"}").ok); // out of the group
        QVERIFY(access.commands.last().text.isEmpty());

        QVERIFY(call("scale_image", "{\"width\":800,\"height\":600.4}").ok);
        QCOMPARE(access.commands.last().size, QSize(800, 600));
    }

    void testBadLayerArgumentsNeverReachTheDocument()
    {
        GuiExecutor executor;
        FakeAccess access;
        ToolRegistry registry;
        addLayerTools(&registry, &executor, &access);
        registry.beginRun(RequestSettings());

        const QList<QPair<QString, QString>> bad {
            {"rename_layer", "{\"layer\":\"A\"}"},
            {"rename_layer", "{\"name\":\"B\"}"},
            {"rename_layer", "{\"layer\":5,\"name\":\"B\"}"},
            {"set_layer_visibility", "{\"layer\":\"A\",\"visible\":\"yes\"}"},
            {"set_layer_opacity", "{\"layer\":\"A\"}"},
            {"move_layer", "{\"layer\":\"A\",\"dx\":1}"},
            {"delete_layer", "{}"},
            {"scale_image", "{\"width\":10}"},
            {"reorder_layer", "{\"layer\":\"A\"}"},
        };
        for (const auto &entry : bad) {
            const ToolResult result = inWorkerThread([&]() { return runTool(registry, entry.first, entry.second); });
            QVERIFY2(!result.ok, qPrintable(entry.first + entry.second));
            QVERIFY2(!result.text.isEmpty(), qPrintable(entry.first + entry.second));
        }
        QVERIFY(access.commands.isEmpty());
    }

    void testBoxSelectTurnsFractionsIntoPixels()
    {
        GuiExecutor executor;
        FakeAccess access; // the document is 200 x 100
        ToolRegistry registry;
        addLayerTools(&registry, &executor, &access);
        registry.beginRun(RequestSettings());
        const auto call = [&](const QString &arguments) {
            return inWorkerThread([&]() { return runTool(registry, "box_select", arguments); });
        };

        ToolResult result = call("{\"left\":0.25,\"top\":0.1,\"right\":0.75,\"bottom\":0.9}");
        QVERIFY2(result.ok, qPrintable(result.text));
        QVERIFY(access.calledInGuiThread);
        QCOMPARE(access.commands.size(), 1);
        QCOMPARE(int(access.commands.last().type), int(LayerCommand::SelectRect));
        QCOMPARE(access.commands.last().rect, QRect(50, 10, 100, 80));

        // the whole picture, and the edges 0 and 1 are allowed
        QVERIFY(call("{\"left\":0,\"top\":0,\"right\":1,\"bottom\":1}").ok);
        QCOMPARE(access.commands.last().rect, QRect(0, 0, 200, 100));

        // fractions of a part of the picture (what ask_about_image looked at)
        result = call("{\"left\":0.5,\"top\":0,\"right\":1,\"bottom\":0.5,\"region\":{\"x\":100,\"y\":0,\"width\":100,\"height\":100}}");
        QVERIFY2(result.ok, qPrintable(result.text));
        QCOMPARE(access.commands.last().rect, QRect(150, 0, 50, 50));
    }

    void testBoxSelectRefusesWhatAreNotFractions()
    {
        GuiExecutor executor;
        FakeAccess access;
        ToolRegistry registry;
        addLayerTools(&registry, &executor, &access);
        registry.beginRun(RequestSettings());

        const QStringList bad {
            "{\"left\":0,\"top\":0,\"right\":1.01,\"bottom\":1}", // above 1
            "{\"left\":0,\"top\":0,\"right\":1,\"bottom\":100}", // pixels
            "{\"left\":120,\"top\":10,\"right\":180,\"bottom\":90}", // pixels
            "{\"left\":-0.1,\"top\":0,\"right\":1,\"bottom\":1}",
            "{\"left\":0.5,\"top\":0,\"right\":0.5,\"bottom\":1}", // no width
            "{\"left\":0.8,\"top\":0,\"right\":0.2,\"bottom\":1}", // upside down
            "{\"left\":0,\"top\":0.9,\"right\":1,\"bottom\":0.1}",
            "{\"left\":0,\"top\":0,\"right\":1}", // missing
            "{\"left\":\"0\",\"top\":0,\"right\":1,\"bottom\":1}", // text
            "{\"left\":0,\"top\":0,\"right\":0.001,\"bottom\":0.001}", // less than a pixel
            "{\"left\":0,\"top\":0,\"right\":1,\"bottom\":1,\"region\":{\"x\":150,\"y\":0,\"width\":100,\"height\":50}}", // outside
        };
        for (const QString &arguments : bad) {
            const ToolResult result = inWorkerThread([&]() { return runTool(registry, "box_select", arguments); });
            QVERIFY2(!result.ok, qPrintable(arguments));
            QVERIFY2(!result.text.isEmpty(), qPrintable(arguments));
        }
        QVERIFY(access.commands.isEmpty()); // nothing was selected

        // the answer for numbers above 1 says what is wrong
        const ToolResult result = inWorkerThread([&]() { return runTool(registry, "box_select", bad[0]); });
        QVERIFY(result.text.contains("fraction"));
        QVERIFY(result.text.contains("1.01"));
    }

    void testWhatTheDocumentRefusesIsToldToTheModel()
    {
        GuiExecutor executor;
        FakeAccess access;
        access.canApply = false; // "no window"
        ToolRegistry registry;
        addLayerTools(&registry, &executor, &access);
        registry.beginRun(RequestSettings());

        const ToolResult result = inWorkerThread([&]() { return runTool(registry, "delete_layer", "{\"layer\":\"A\"}"); });
        QVERIFY(!result.ok);
        QCOMPARE(result.text, QString("no window"));
    }

    // ---- the Topaz tools (fal.ai) ----

    void testTopazToolsAreOnlyOfferedWithAKey()
    {
        GuiExecutor executor;
        FakeAccess access;
        FalClient fal;
        Progress progress;
        ToolRegistry registry;
        addFalTools(&registry, &executor, &access, &fal, &progress);

        registry.beginRun(RequestSettings());
        QCOMPARE(registry.toolDefinitions().size(), 0);

        RequestSettings withKey;
        withKey.fal.apiKey = "k";
        registry.beginRun(withKey);
        QStringList names;
        for (const QJsonValue &entry : registry.toolDefinitions()) {
            names << entry.toObject()["function"].toObject()["name"].toString();
        }
        names.sort();
        QCOMPARE(names, (QStringList {"denoise_image", "upscale_image"}));
    }

    void testUpscaleSendsThePictureAndOpensTheResultAsANewDocument()
    {
        FakeServer server;
        QVERIFY(server.start());
        scriptFalJob(server);

        GuiExecutor executor;
        FakeAccess access;
        FalClient fal;
        Progress progress;
        ToolRegistry registry;
        addFalTools(&registry, &executor, &access, &fal, &progress);
        registry.beginRun(settingsWithFal(server));

        // is the modal window up while the job runs?
        bool windowSeen = false;
        QTimer watch;
        connect(&watch, &QTimer::timeout, &watch, [&]() { windowSeen = windowSeen || progress.isActive(); });
        watch.start(10);

        const ToolResult result = inWorkerThread([&]() {
            return runTool(registry, "upscale_image", "{\"upscale_factor\":3,\"model\":\"High Fidelity V3\",\"sharpen\":0.5}");
        });
        watch.stop();

        QVERIFY2(result.ok, qPrintable(result.text));
        QVERIFY(windowSeen);
        QTRY_VERIFY(!progress.isActive()); // and it is gone again

        // the job sent to fal.ai
        QVERIFY(server.requests.size() >= 3);
        // first the picture goes into the file storage, at full size: a data URI inside the job is too big
        QCOMPARE(server.requests[0].path, QByteArray("/storage/upload/initiate?storage_type=fal-cdn-v3"));
        QCOMPARE(server.requests[0].headers.value("authorization"), QByteArray("Key fal-secret"));
        QCOMPARE(server.requests[1].path, QByteArray("/upload/abc"));
        QVERIFY(!server.requests[1].headers.contains("authorization"));
        QCOMPARE(server.requests[1].headers.value("content-type"), QByteArray("image/png"));
        const QImage sent = QImage::fromData(server.requests[1].body, "PNG");
        QCOMPARE(sent.size(), QSize(200, 100)); // not shrunk like for the vision model

        // then the job, which only names the picture
        QCOMPARE(server.requests[2].path, QByteArray("/topaz/upscale/image/precision"));
        QCOMPARE(server.requests[2].headers.value("authorization"), QByteArray("Key fal-secret"));
        const QJsonObject job = server.requests[2].json();
        QCOMPARE(job["image_url"].toString(), server.baseUrl().toString() + "/files/in.png");
        QCOMPARE(job["upscale_factor"].toDouble(), 3.0);
        QCOMPARE(job["model"].toString(), QString("High Fidelity V3"));
        QCOMPARE(job["sharpen"].toDouble(), 0.5);
        QCOMPARE(job["output_format"].toString(), QString("png")); // keeps transparency

        // when everything is done the uploaded picture is deleted from the storage again
        QCOMPARE(server.requests.size(), 8);
        QCOMPARE(server.requests.last().path, QByteArray("/storage/files/delete"));
        QCOMPARE(server.requests.last().headers.value("authorization"), QByteArray("Key fal-secret"));
        QCOMPARE(server.requests.last().json()["file_url"].toString(), server.baseUrl().toString() + "/files/in.png");

        // The whole picture, made 3 times bigger: the document grows, then the result is a layer that fits it.
        QVERIFY(access.calledInGuiThread);
        QCOMPARE(access.commands.size(), 2);
        QCOMPARE(int(access.commands[0].type), int(LayerCommand::ScaleImage));
        QCOMPARE(access.commands[0].size, QSize(600, 300));
        QCOMPARE(int(access.commands[1].type), int(LayerCommand::AddImageLayer));
        QCOMPARE(access.commands[1].text, QString("Upscaled x3"));
        QCOMPARE(access.commands[1].point, QPoint(0, 0));
        QCOMPARE(access.commands[1].image.size(), QSize(600, 300)); // fitted: fal.ai made 40 x 20 here
        QVERIFY(result.text.contains("new layer") || result.text.contains("on top"));
        QVERIFY(result.text.contains("undo"));
    }

    void testDenoiseUsesItsOwnEndpoint()
    {
        FakeServer server;
        QVERIFY(server.start());
        scriptFalJob(server, "denoise");

        GuiExecutor executor;
        FakeAccess access;
        access.renderedRegion = QRect(10, 20, 50, 40); // "the selection"
        FalClient fal;
        Progress progress;
        ToolRegistry registry;
        addFalTools(&registry, &executor, &access, &fal, &progress);
        registry.beginRun(settingsWithFal(server));

        const ToolResult result = inWorkerThread([&]() {
            return runTool(registry, "denoise_image", "{\"model\":\"Strong\",\"selection_only\":true}");
        });
        QVERIFY2(result.ok, qPrintable(result.text));
        QCOMPARE(server.requests[2].path, QByteArray("/topaz/denoise/image"));
        QCOMPARE(server.requests[2].json()["model"].toString(), QString("Strong"));
        QVERIFY(access.lastSelectionOnly);
        // a part of the picture goes back to where it came from, at its size; the document stays as it is
        QCOMPARE(access.commands.size(), 1);
        QCOMPARE(int(access.commands[0].type), int(LayerCommand::AddImageLayer));
        QCOMPARE(access.commands[0].text, QString("Denoised"));
        QCOMPARE(access.commands[0].point, QPoint(10, 20));
        QCOMPARE(access.commands[0].image.size(), QSize(50, 40));
    }

    void testUpscalingAPartKeepsTheDocumentSize()
    {
        FakeServer server;
        QVERIFY(server.start());
        scriptFalJob(server);

        GuiExecutor executor;
        FakeAccess access;
        access.renderedRegion = QRect(30, 10, 100, 50);
        FalClient fal;
        Progress progress;
        ToolRegistry registry;
        addFalTools(&registry, &executor, &access, &fal, &progress);
        registry.beginRun(settingsWithFal(server));

        const ToolResult result = inWorkerThread([&]() { return runTool(registry, "upscale_image", "{\"upscale_factor\":2}"); });
        QVERIFY2(result.ok, qPrintable(result.text));
        QCOMPARE(access.commands.size(), 1); // no ScaleImage
        QCOMPARE(access.commands[0].point, QPoint(30, 10));
        QCOMPARE(access.commands[0].image.size(), QSize(100, 50));
    }

    void testBadTopazArgumentsAreRefusedBeforeAnythingIsSent()
    {
        FakeServer server;
        QVERIFY(server.start());

        GuiExecutor executor;
        FakeAccess access;
        FalClient fal;
        Progress progress;
        ToolRegistry registry;
        addFalTools(&registry, &executor, &access, &fal, &progress);
        registry.beginRun(settingsWithFal(server));

        const QStringList bad {
            "{\"upscale_factor\":0}", "{\"upscale_factor\":99}", "{\"upscale_factor\":\"big\"}",
            "{\"model\":\"Turbo\"}", "{\"sharpen\":2}", "{\"denoise\":-1}", "{\"region\":5}",
        };
        for (const QString &arguments : bad) {
            const ToolResult result = inWorkerThread([&]() { return runTool(registry, "upscale_image", arguments); });
            QVERIFY2(!result.ok, qPrintable(arguments));
            QVERIFY2(!result.text.isEmpty(), qPrintable(arguments));
        }
        const ToolResult badModel = inWorkerThread([&]() { return runTool(registry, "denoise_image", "{\"model\":\"Standard V2\"}"); });
        QVERIFY(!badModel.ok);

        QCOMPARE(server.requests.size(), 0);
        QVERIFY(access.commands.isEmpty());
    }

    void testAFailedJobIsReportedAndOpensNothing()
    {
        FakeServer server;
        QVERIFY(server.start());
        ScriptedResponse rejected;
        rejected.status = 401;
        rejected.contentType = "application/json";
        rejected.chunks = {"{\"detail\":\"Invalid key\"}"};
        server.enqueue(rejected);

        GuiExecutor executor;
        FakeAccess access;
        FalClient fal;
        Progress progress;
        ToolRegistry registry;
        addFalTools(&registry, &executor, &access, &fal, &progress);
        registry.beginRun(settingsWithFal(server));

        const ToolResult result = inWorkerThread([&]() { return runTool(registry, "upscale_image", "{}"); });
        QVERIFY(!result.ok);
        QVERIFY(result.text.contains("fal.ai failed"));
        QVERIFY(access.commands.isEmpty());
        QTRY_VERIFY(!progress.isActive()); // the window does not stay on the screen
    }

    void testWhenTheUploadFailsThePictureTravelsInsideTheJob()
    {
        FakeServer server;
        QVERIFY(server.start());
        ScriptedResponse refused; // the storage does not know this way of uploading
        refused.status = 404;
        refused.contentType = "application/json";
        refused.chunks = {"{\"detail\":\"Not Found\"}"};
        server.enqueue(refused);
        // the job itself goes on as always
        server.enqueue(falJson(falSubmitted(server, "upscale")));
        QJsonObject done;
        done["status"] = "COMPLETED";
        server.enqueue(falJson(done));
        QJsonObject image;
        image["url"] = QString(server.baseUrl().toString() + "/files/result.png");
        QJsonObject result;
        result["image"] = image;
        server.enqueue(falJson(result));
        QImage picture(40, 20, QImage::Format_ARGB32);
        picture.fill(Qt::green);
        ScriptedResponse file;
        file.contentType = "image/png";
        file.chunks = {VisionClient::encodePng(picture)};
        server.enqueue(file);

        GuiExecutor executor;
        FakeAccess access;
        FalClient fal;
        Progress progress;
        ToolRegistry registry;
        addFalTools(&registry, &executor, &access, &fal, &progress);
        registry.beginRun(settingsWithFal(server));

        const ToolResult result2 = inWorkerThread([&]() { return runTool(registry, "denoise_image", "{}"); });
        QVERIFY2(result2.ok, qPrintable(result2.text));
        QCOMPARE(server.requests[1].path, QByteArray("/topaz/denoise/image"));
        QVERIFY(server.requests[1].json()["image_url"].toString().startsWith("data:image/png;base64,"));
        QCOMPARE(access.commands.size(), 1);
        // nothing was uploaded, so there is nothing to delete
        for (const RecordedRequest &request : server.requests) {
            QVERIFY(!request.path.contains("delete"));
        }
    }

    void testNoDocumentMeansNoJob()
    {
        FakeServer server;
        QVERIFY(server.start());

        GuiExecutor executor;
        FakeAccess access;
        access.hasDocument = false;
        FalClient fal;
        Progress progress;
        ToolRegistry registry;
        addFalTools(&registry, &executor, &access, &fal, &progress);
        registry.beginRun(settingsWithFal(server));

        const ToolResult result = inWorkerThread([&]() { return runTool(registry, "upscale_image", "{}"); });
        QVERIFY(!result.ok);
        QVERIFY(result.text.contains("no open document"));
        QCOMPARE(server.requests.size(), 0);
    }

    void testTheCancelButtonStopsTheJobAndTellsTheModel()
    {
        FakeServer server;
        QVERIFY(server.start());
        scriptFalUpload(server);
        server.enqueue(falJson(falSubmitted(server, "upscale")));
        ScriptedResponse silent; // fal.ai never says anything about the status
        silent.contentType = "text/event-stream";
        silent.chunks = {"x"};
        silent.hangAfterChunks = true;
        server.enqueue(silent);

        GuiExecutor executor;
        FakeAccess access;
        FalClient fal;
        Progress progress;
        // as the panel does it
        connect(&progress, &Progress::cancelRequested, &fal, [&]() { fal.cancel(); }, Qt::DirectConnection);
        ToolRegistry registry;
        addFalTools(&registry, &executor, &access, &fal, &progress);
        registry.beginRun(settingsWithFal(server));

        // the user presses Cancel once the window is up
        QTimer press;
        connect(&press, &QTimer::timeout, &press, [&]() {
            if (progress.isActive() && server.requests.size() >= 4) { // upload (2), job, status
                press.stop();
                emit progress.cancelRequested();
            }
        });
        press.start(10);

        const ToolResult result = inWorkerThread([&]() { return runTool(registry, "upscale_image", "{}"); });
        press.stop();
        QVERIFY(!result.ok);
        QVERIFY(result.text.contains("canceled"));
        QVERIFY(access.commands.isEmpty());
        // a canceled job still cleans up after itself
        QCOMPARE(server.requests.last().path, QByteArray("/storage/files/delete"));
        QTRY_VERIFY(!progress.isActive());
    }
};

QTEST_MAIN(ToolsTest)

#include "ToolsTest.moc"

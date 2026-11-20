/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QProgressDialog>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <memory>

#include "../ai/tools/Fal.h"
#include "../ui/Progress.h"
#include "../ai/tools/Vision.h"
#include "FakeOpenAiServer.h"

using namespace AiChat;

namespace {

ScriptedResponse json(const QJsonObject &object, int status = 200)
{
    ScriptedResponse response;
    response.status = status;
    response.contentType = "application/json";
    response.chunks = {QJsonDocument(object).toJson(QJsonDocument::Compact)};
    return response;
}

QJsonObject statusObject(const QString &state, int position = -1)
{
    QJsonObject object;
    object["status"] = state;
    if (position >= 0) {
        object["queue_position"] = position;
    }
    return object;
}

QByteArray pngBytes()
{
    QImage image(40, 20, QImage::Format_ARGB32);
    image.fill(Qt::green);
    return VisionClient::encodePng(image);
}

ScriptedResponse pictureReply()
{
    ScriptedResponse response;
    response.contentType = "image/png";
    response.chunks = {pngBytes()};
    return response;
}

/// What the server answers when a job is accepted. The addresses point back at the fake server.
QJsonObject submitted(const FakeServer &server)
{
    QJsonObject object;
    object["request_id"] = "req-1";
    object["status_url"] = QString(server.baseUrl().toString() + "/topaz/upscale/requests/req-1/status");
    object["response_url"] = QString(server.baseUrl().toString() + "/topaz/upscale/requests/req-1");
    return object;
}

QJsonObject resultObject(const FakeServer &server)
{
    QJsonObject image;
    image["url"] = QString(server.baseUrl().toString() + "/files/result.png");
    image["content_type"] = "image/png";
    QJsonObject object;
    object["image"] = image;
    return object;
}

/// A whole successful job: accepted, queued, working, done, result, picture.
void scriptSuccessfulJob(FakeServer &server)
{
    server.enqueue(json(submitted(server)));
    server.enqueue(json(statusObject("IN_QUEUE", 0)));
    server.enqueue(json(statusObject("IN_PROGRESS")));
    server.enqueue(json(statusObject("COMPLETED")));
    server.enqueue(json(resultObject(server)));
    server.enqueue(pictureReply());
}

FalSettings settingsFor(const FakeServer &server)
{
    FalSettings settings;
    settings.apiKey = "fal-secret";
    settings.queueUrl = server.baseUrl();
    return settings;
}

} // namespace

class FalTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testASuccessfulJob()
    {
        FakeServer server;
        QVERIFY(server.start());
        scriptSuccessfulJob(server);

        QJsonObject input;
        input["image_url"] = "data:image/png;base64,AAAA";
        input["upscale_factor"] = 3;

        QStringList texts;
        FalClient client;
        QString error;
        const QByteArray bytes = client.run(settingsFor(server), "topaz/upscale/image/precision", input,
                                            [&](const QString &text) { texts << text; }, &error, 20000, 10);
        QVERIFY2(!bytes.isEmpty(), qPrintable(error));
        QCOMPARE(bytes, pngBytes());

        QCOMPARE(server.requests.size(), 6);
        // the job: to the queue, with the key, with what was asked for
        QCOMPARE(server.requests[0].path, QByteArray("/topaz/upscale/image/precision"));
        QCOMPARE(server.requests[0].headers.value("authorization"), QByteArray("Key fal-secret"));
        QCOMPARE(server.requests[0].json().value("upscale_factor").toInt(), 3);
        QCOMPARE(server.requests[0].json().value("image_url").toString(), QString("data:image/png;base64,AAAA"));
        // then the addresses the server gave
        QCOMPARE(server.requests[1].path, QByteArray("/topaz/upscale/requests/req-1/status"));
        QCOMPARE(server.requests[4].path, QByteArray("/topaz/upscale/requests/req-1"));
        QCOMPARE(server.requests[5].path, QByteArray("/files/result.png"));
        // the picture is fetched without the key
        QVERIFY(!server.requests[5].headers.contains("authorization"));

        // the user was told what happens, in order
        QVERIFY(texts.first().contains("Sending"));
        QVERIFY(texts.join("|").contains("queue"));
        QVERIFY(texts.join("|").contains("working"));
        QVERIFY(texts.last().contains("Downloading"));
    }

    void testDocumentedAddressesAreUsedWhenTheServerSendsNone()
    {
        const QUrl queue("https://queue.fal.run");
        QJsonObject accepted;
        accepted["request_id"] = "abc";
        // the status and the result live under "topaz/upscale", not under the whole endpoint
        QCOMPARE(FalClient::statusUrl(queue, "topaz/upscale/image/precision", accepted).toString(),
                 QString("https://queue.fal.run/topaz/upscale/requests/abc/status"));
        QCOMPARE(FalClient::resultUrl(queue, "topaz/denoise/image", accepted).toString(),
                 QString("https://queue.fal.run/topaz/denoise/requests/abc"));
        // addresses the server sent win
        accepted["status_url"] = "https://queue.fal.run/x/requests/abc/status";
        QCOMPARE(FalClient::statusUrl(queue, "topaz/upscale/image/precision", accepted).toString(),
                 QString("https://queue.fal.run/x/requests/abc/status"));
    }

    void testStatusTexts()
    {
        QVERIFY(FalClient::statusText(statusObject("IN_QUEUE", 2)).contains("3")); // position 2 counts from 0
        QVERIFY(FalClient::statusText(statusObject("IN_QUEUE")).contains("queue"));
        QVERIFY(FalClient::statusText(statusObject("IN_PROGRESS")).contains("working"));
        QVERIFY(!FalClient::statusText(statusObject("SOMETHING_NEW")).isEmpty());
    }

    void testARejectedKeyIsExplained()
    {
        FakeServer server;
        QVERIFY(server.start());
        QJsonObject detail;
        detail["detail"] = "Invalid key";
        server.enqueue(json(detail, 401));

        FalClient client;
        QString error;
        const QByteArray bytes = client.run(settingsFor(server), "topaz/denoise/image", QJsonObject(), nullptr, &error, 20000, 10);
        QVERIFY(bytes.isEmpty());
        QVERIFY(!error.isEmpty());
        QCOMPARE(server.requests.size(), 1); // nothing else was tried
    }

    void testNoKeyMeansNoRequest()
    {
        FakeServer server;
        QVERIFY(server.start());
        FalSettings settings = settingsFor(server);
        settings.apiKey.clear();

        FalClient client;
        QString error;
        QVERIFY(client.run(settings, "topaz/denoise/image", QJsonObject(), nullptr, &error).isEmpty());
        QVERIFY(error.contains("key"));
        QCOMPARE(server.requests.size(), 0);
    }

    void testAResultWithoutPictureIsAnError()
    {
        FakeServer server;
        QVERIFY(server.start());
        server.enqueue(json(submitted(server)));
        server.enqueue(json(statusObject("COMPLETED")));
        server.enqueue(json(QJsonObject())); // no "image"

        FalClient client;
        QString error;
        QVERIFY(client.run(settingsFor(server), "topaz/upscale/image/precision", QJsonObject(), nullptr, &error, 20000, 10).isEmpty());
        QVERIFY(error.contains("no picture"));
    }

    void testCancelingStopsTheJob()
    {
        FakeServer server;
        QVERIFY(server.start());
        server.enqueue(json(submitted(server)));
        server.enqueue(json(statusObject("IN_QUEUE", 0)));
        for (int i = 0; i < 20; ++i) {
            server.enqueue(json(statusObject("IN_QUEUE", 0)));
        }

        FalClient client;
        QString error;
        QElapsedTimer timer;
        timer.start();
        const QByteArray bytes = client.run(settingsFor(server), "topaz/upscale/image/precision", QJsonObject(),
                                            [&](const QString &text) {
                                                if (text.contains("queue")) {
                                                    client.cancel(); // like the Cancel button
                                                }
                                            }, &error, 20000, 10000);
        QVERIFY(bytes.isEmpty());
        QCOMPARE(error, QString("Canceled."));
        QVERIFY2(timer.elapsed() < 5000, "cancel must not wait for the next poll");
    }

    void testCancelingWhileTheServerIsSilent()
    {
        FakeServer server;
        QVERIFY(server.start());
        server.enqueue(json(submitted(server)));
        ScriptedResponse silent; // the status request is never answered
        silent.contentType = "text/event-stream";
        silent.chunks = {"x"};
        silent.hangAfterChunks = true;
        server.enqueue(silent);

        FalClient client;
        QTimer::singleShot(300, &client, [&]() { client.cancel(); });
        QString error;
        QElapsedTimer timer;
        timer.start();
        QVERIFY(client.run(settingsFor(server), "topaz/upscale/image/precision", QJsonObject(), nullptr, &error, 20000, 10).isEmpty());
        QCOMPARE(error, QString("Canceled."));
        QVERIFY(timer.elapsed() < 5000);
    }

    void testATimeoutEnds()
    {
        FakeServer server;
        QVERIFY(server.start());
        server.enqueue(json(submitted(server)));
        for (int i = 0; i < 200; ++i) {
            server.enqueue(json(statusObject("IN_PROGRESS")));
        }

        FalClient client;
        QString error;
        QVERIFY(client.run(settingsFor(server), "topaz/upscale/image/precision", QJsonObject(), nullptr, &error, 400, 20).isEmpty());
        QVERIFY(error.contains("in time"));
    }

    // ---- the modal window ----

    void testTheProgressWindowIsModalAndShowsTheState()
    {
        Progress progress;
        QVERIFY(!progress.isActive());

        // as the worker thread does it: only events are posted
        std::unique_ptr<QThread> thread(QThread::create([&]() {
            progress.beginLater("Sending the image");
            progress.updateLater("Topaz is working");
        }));
        thread->start();
        QTRY_VERIFY(progress.isActive());
        thread->wait();

        QProgressDialog *dialog = nullptr;
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == "aiChatProgress") {
                dialog = qobject_cast<QProgressDialog *>(widget);
            }
        }
        QVERIFY(dialog);
        QCOMPARE(dialog->windowModality(), Qt::ApplicationModal);
        QCOMPARE(dialog->minimum(), 0);
        QCOMPARE(dialog->maximum(), 0); // a bar that only moves: the end is not known
        QTRY_VERIFY(dialog->labelText().contains("Topaz is working"));
        QVERIFY(dialog->labelText().contains(" s")); // how long it takes already

        progress.endLater();
        QTRY_VERIFY(!progress.isActive());
    }

    void testCancelButtonIsReported()
    {
        Progress progress;
        QSignalSpy spy(&progress, &Progress::cancelRequested);
        progress.begin("Working");
        QProgressDialog *dialog = nullptr;
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == "aiChatProgress" && widget->isVisible()) {
                dialog = qobject_cast<QProgressDialog *>(widget);
            }
        }
        QVERIFY(dialog);
        QPushButton *button = dialog->findChild<QPushButton *>();
        QVERIFY(button);
        button->click();
        QCOMPARE(spy.count(), 1);
    }

    void testClosingTheWindowFromOutsideIsNotACancel()
    {
        Progress progress;
        QSignalSpy spy(&progress, &Progress::cancelRequested);
        progress.begin("Working");
        QVERIFY(progress.isActive());
        progress.end();
        QVERIFY(!progress.isActive());
        progress.end(); // twice is fine
        QCOMPARE(spy.count(), 0);
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    FalTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "FalTest.moc"

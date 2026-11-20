/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Worker.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>

#include <klocalizedstring.h>

#include "../ai/Agent.h"
#include "Store.h"

namespace AiChat {

Worker::Worker(const QString &databasePath, QObject *parent)
    : QObject(parent)
    , m_databasePath(databasePath)
{
    qRegisterMetaType<StoredMessage>("StoredMessage"); // plain names: queued calls look types up by the name moc writes
    qRegisterMetaType<QList<StoredMessage>>("QList<StoredMessage>");
    qRegisterMetaType<RequestSettings>("RequestSettings");
}

Worker::~Worker()
{
}

QString Worker::defaultDatabasePath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("aichat/chat.sqlite"));
}

void Worker::setToolProvider(AgentToolProvider *provider)
{
    m_toolProvider = provider;
    if (m_agent) {
        m_agent->setToolProvider(provider);
    }
}

void Worker::setMaxToolRounds(int rounds)
{
    m_maxToolRounds = rounds;
    if (m_agent) {
        m_agent->setMaxRounds(rounds);
    }
}

void Worker::init()
{
    // Created here, so that they belong to the thread this runs in.
    m_agent = new Agent(this);
    m_agent->setToolProvider(m_toolProvider);
    m_agent->setMaxRounds(m_maxToolRounds);
    connect(m_agent, &Agent::textDelta, this, &Worker::assistantDelta);
    connect(m_agent, &Agent::finished, this, &Worker::onAgentFinished);
    connect(m_agent, &Agent::failed, this, &Worker::onAgentFailed);
    connect(m_agent, &Agent::canceled, this, &Worker::onAgentCanceled);

    m_store = std::make_unique<Store>(m_databasePath);
    if (m_store->open()) {
        m_conversationId = m_store->latestConversationId();
        if (m_conversationId < 0) {
            m_conversationId = m_store->createConversation();
        }
        m_history = m_store->messages(m_conversationId);
    } else {
        Q_EMIT storageError(m_store->lastError());
    }

    Q_EMIT conversationLoaded(m_history);
}

void Worker::shutdown()
{
    m_newConversationAfterRequest = false;
    if (m_agent && m_agent->isRunning()) {
        m_agent->cancel(); // a partial answer is still saved
    }

    // The network manager and the database connection belong to this thread: release them here,
    // while we are still in it. After this the worker answers every request with "not ready".
    delete m_agent;
    m_agent = nullptr;
    m_store.reset();
}

void Worker::submit(const QString &text, const RequestSettings &settings)
{
    if (!m_agent) {
        Q_EMIT requestFailed(i18n("The AI chat is not ready yet."));
        return;
    }
    if (m_busy) {
        Q_EMIT requestFailed(i18n("Still waiting for the previous answer."));
        return;
    }

    setBusy(true);
    m_requestTimer.start();

    StoredMessage user;
    user.role = QStringLiteral("user");
    user.content = text;
    user.createdAt = QDateTime::currentDateTime();
    persist(&user);
    m_history.append(user);

    // the whole conversation is the context of the request
    QJsonArray conversation;
    for (const StoredMessage &message : std::as_const(m_history)) {
        QJsonObject entry;
        entry["role"] = message.role;
        entry["content"] = message.content;
        conversation.append(entry);
    }

    m_agent->start(settings, conversation);
}

void Worker::cancel()
{
    if (m_agent && m_agent->isRunning()) {
        m_agent->cancel();
    }
}

void Worker::startNewConversation()
{
    if (m_busy) {
        // The canceled request still belongs to the old conversation; the new one starts when it is over.
        m_newConversationAfterRequest = true;
        cancel();
        return;
    }
    createConversation();
}

void Worker::createConversation()
{
    // An empty conversation is simply reused: a pile of empty ones would only clutter the database.
    if (m_history.isEmpty() && m_conversationId >= 0) {
        return;
    }

    m_history.clear();
    if (m_store && m_store->isOpen()) {
        m_conversationId = m_store->createConversation();
    }
}

void Worker::persist(StoredMessage *message)
{
    if (!m_store || !m_store->isOpen() || m_conversationId < 0) {
        return;
    }

    message->id = m_store->appendMessage(m_conversationId, message->role, message->content, message->createdAt, message->processingMs,
                                         message->tokens);
    if (message->id < 0) {
        Q_EMIT storageError(m_store->lastError());
    }
}

void Worker::saveAndAnnounceAnswer(const QString &text)
{
    StoredMessage answer;
    answer.role = QStringLiteral("assistant");
    answer.content = text;
    answer.createdAt = QDateTime::currentDateTime();
    answer.processingMs = m_requestTimer.elapsed();
    answer.tokens = m_agent ? m_agent->totalTokens() : -1;
    persist(&answer);
    m_history.append(answer);

    Q_EMIT assistantFinished(answer);
}

void Worker::onAgentFinished(const QString &text)
{
    if (text.trimmed().isEmpty()) {
        Q_EMIT requestFailed(i18n("The AI sent an empty answer."));
    } else {
        saveAndAnnounceAnswer(text);
    }
    endRequest();
}

void Worker::onAgentFailed(const QString &error, const QString &partialText)
{
    // what already arrived is kept: it was shown, so it is saved too
    if (!partialText.trimmed().isEmpty()) {
        saveAndAnnounceAnswer(partialText);
    }
    Q_EMIT requestFailed(error);
    endRequest();
}

void Worker::onAgentCanceled(const QString &partialText)
{
    if (!partialText.trimmed().isEmpty()) {
        saveAndAnnounceAnswer(partialText);
    }
    Q_EMIT requestCanceled();
    endRequest();
}

void Worker::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    Q_EMIT busyChanged(busy);
}

void Worker::endRequest()
{
    setBusy(false);

    if (m_newConversationAfterRequest) {
        m_newConversationAfterRequest = false;
        createConversation();
    }
}

} // namespace AiChat

/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AiChatWorker.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>

#include <klocalizedstring.h>

#include "AiChatAgent.h"
#include "AiChatStore.h"

AiChatWorker::AiChatWorker(const QString &databasePath, QObject *parent)
    : QObject(parent)
    , m_databasePath(databasePath)
{
    qRegisterMetaType<AiChatStoredMessage>();
    qRegisterMetaType<QList<AiChatStoredMessage>>();
    qRegisterMetaType<AiChatRequestSettings>();
}

AiChatWorker::~AiChatWorker()
{
}

QString AiChatWorker::defaultDatabasePath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("aichat/chat.sqlite"));
}

void AiChatWorker::setToolProvider(AiChatToolProvider *provider)
{
    m_toolProvider = provider;
    if (m_agent) {
        m_agent->setToolProvider(provider);
    }
}

void AiChatWorker::setMaxToolRounds(int rounds)
{
    m_maxToolRounds = rounds;
    if (m_agent) {
        m_agent->setMaxRounds(rounds);
    }
}

void AiChatWorker::init()
{
    // Created here, so that they belong to the thread this runs in.
    m_agent = new AiChatAgent(this);
    m_agent->setToolProvider(m_toolProvider);
    m_agent->setMaxRounds(m_maxToolRounds);
    connect(m_agent, &AiChatAgent::textDelta, this, &AiChatWorker::assistantDelta);
    connect(m_agent, &AiChatAgent::finished, this, &AiChatWorker::onAgentFinished);
    connect(m_agent, &AiChatAgent::failed, this, &AiChatWorker::onAgentFailed);
    connect(m_agent, &AiChatAgent::canceled, this, &AiChatWorker::onAgentCanceled);

    m_store = std::make_unique<AiChatStore>(m_databasePath);
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

void AiChatWorker::shutdown()
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

void AiChatWorker::submit(const QString &text, const AiChatRequestSettings &settings)
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

    AiChatStoredMessage user;
    user.role = QStringLiteral("user");
    user.content = text;
    user.createdAt = QDateTime::currentDateTime();
    persist(&user);
    m_history.append(user);

    // the whole conversation is the context of the request
    QJsonArray conversation;
    for (const AiChatStoredMessage &message : std::as_const(m_history)) {
        QJsonObject entry;
        entry["role"] = message.role;
        entry["content"] = message.content;
        conversation.append(entry);
    }

    m_agent->start(settings, conversation);
}

void AiChatWorker::cancel()
{
    if (m_agent && m_agent->isRunning()) {
        m_agent->cancel();
    }
}

void AiChatWorker::startNewConversation()
{
    if (m_busy) {
        // The canceled request still belongs to the old conversation; the new one starts when it is over.
        m_newConversationAfterRequest = true;
        cancel();
        return;
    }
    createConversation();
}

void AiChatWorker::createConversation()
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

void AiChatWorker::persist(AiChatStoredMessage *message)
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

void AiChatWorker::saveAndAnnounceAnswer(const QString &text)
{
    AiChatStoredMessage answer;
    answer.role = QStringLiteral("assistant");
    answer.content = text;
    answer.createdAt = QDateTime::currentDateTime();
    answer.processingMs = m_requestTimer.elapsed();
    answer.tokens = m_agent ? m_agent->totalTokens() : -1;
    persist(&answer);
    m_history.append(answer);

    Q_EMIT assistantFinished(answer);
}

void AiChatWorker::onAgentFinished(const QString &text)
{
    if (text.trimmed().isEmpty()) {
        Q_EMIT requestFailed(i18n("The AI sent an empty answer."));
    } else {
        saveAndAnnounceAnswer(text);
    }
    endRequest();
}

void AiChatWorker::onAgentFailed(const QString &error, const QString &partialText)
{
    // what already arrived is kept: it was shown, so it is saved too
    if (!partialText.trimmed().isEmpty()) {
        saveAndAnnounceAnswer(partialText);
    }
    Q_EMIT requestFailed(error);
    endRequest();
}

void AiChatWorker::onAgentCanceled(const QString &partialText)
{
    if (!partialText.trimmed().isEmpty()) {
        saveAndAnnounceAnswer(partialText);
    }
    Q_EMIT requestCanceled();
    endRequest();
}

void AiChatWorker::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    Q_EMIT busyChanged(busy);
}

void AiChatWorker::endRequest()
{
    setBusy(false);

    if (m_newConversationAfterRequest) {
        m_newConversationAfterRequest = false;
        createConversation();
    }
}

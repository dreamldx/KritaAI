/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Dock.h"

#include <QAbstractButton>
#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QActionGroup>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPalette>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <klocalizedstring.h>

#include "../ai/tools/DocumentTools.h"
#include "../model/Chat.h"
#include "MessageWidget.h"
#include "Progress.h"

namespace AiChat {

namespace {

QString effortName(ReasoningEffort effort)
{
    switch (effort) {
    case ReasoningEffort::Low:
        return i18n("Low");
    case ReasoningEffort::High:
        return i18n("High");
    case ReasoningEffort::Medium:
    default:
        return i18n("Medium");
    }
}

} // namespace
Dock::Dock(const QString &databasePath)
    : QDockWidget()
    , m_scrollArea(new QScrollArea())
    , m_messagesContainer(new QWidget())
    , m_messagesLayout(new QVBoxLayout(m_messagesContainer))
    , m_emptyHint(new QLabel(i18n("No messages yet.\nType below to start a conversation.")))
    , m_composer(new QFrame())
    , m_input(new QPlainTextEdit())
{
    setWindowTitle(i18n("AI Chat"));

    // The state of the conversation, the worker thread and the tools live in the model: this panel only shows it.
    m_documentAccess = new KritaDocumentAccess();
    m_progress = new Progress(this); // the modal window of the slow tools; lives in the GUI thread
    m_chat = new Chat(databasePath, m_documentAccess, m_progress, this);
    connect(m_chat, &Chat::reasoningEffortChanged, this, [this](ReasoningEffort effort) {
        updateEffortButton(effort);
        Q_EMIT reasoningEffortChanged(effort);
    });
    connect(m_chat, &Chat::busyChanged, this, &Dock::slotBusyChanged);
    connect(m_chat, &Chat::historyLoaded, this, &Dock::slotHistoryLoaded);
    connect(m_chat, &Chat::assistantDelta, this, &Dock::slotAssistantDelta);
    connect(m_chat, &Chat::assistantFinished, this, &Dock::slotAssistantFinished);
    connect(m_chat, &Chat::requestFailed, this, &Dock::slotRequestFailed);
    connect(m_chat, &Chat::requestCanceled, this, &Dock::slotRequestCanceled);
    connect(m_chat, &Chat::errorReported, this, &Dock::addErrorMessage);

    // history
    m_messagesLayout->setContentsMargins(kSideMargin, 6, kSideMargin, 6);
    m_messagesLayout->setSpacing(14);

    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setWordWrap(true);
    m_emptyHint->setEnabled(false); // draws in the disabled (dimmed) text color
    m_messagesLayout->addWidget(m_emptyHint);
    m_messagesLayout->addStretch(1);

    m_scrollArea->setWidget(m_messagesContainer);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // no outer frame of the history, whatever the theme's style sheet says
    m_scrollArea->setStyleSheet("QScrollArea { border: none; background: transparent; }"
                                "QScrollArea > QWidget > QWidget { background: transparent; }");

    // Keep following the newest message, unless the user scrolled up.
    QScrollBar *bar = m_scrollArea->verticalScrollBar();
    connect(bar, &QScrollBar::valueChanged, this, [this, bar](int value) {
        m_followBottom = (value >= bar->maximum());
    });
    connect(bar, &QScrollBar::rangeChanged, this, [this, bar](int, int max) {
        if (m_followBottom) {
            bar->setValue(max);
        }
    });

    // composer: a rounded box with the text input on top and a toolbar below
    m_composer->setObjectName("aiChatComposer");

    m_input->setPlaceholderText(i18n("Message the AI... (Shift+Enter for a new line)"));
    m_input->setTabChangesFocus(true);
    m_input->setFrameShape(QFrame::NoFrame);
    m_input->installEventFilter(this);

    // toolbar, left: new chat, add image
    m_newChatButton = new IconButton(IconButton::NewChat, i18n("New chat"));
    m_newChatButton->setEnabled(false); // nothing to clear yet
    m_addImageButton = new IconButton(IconButton::Image, i18n("Add image"));

    // toolbar, right: model settings (thinking effort), then the round send button
    m_effortButton = new QToolButton();
    m_effortButton->setPopupMode(QToolButton::InstantPopup);
    m_effortButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_effortButton->setCursor(Qt::PointingHandCursor);
    m_effortButton->setToolTip(i18n("AI model settings: how much the model thinks before answering"));

    QMenu *effortMenu = new QMenu(m_effortButton);
    effortMenu->addSection(i18n("Thinking effort"));
    QActionGroup *effortGroup = new QActionGroup(effortMenu);
    const ReasoningEffort efforts[] = {ReasoningEffort::Low, ReasoningEffort::Medium, ReasoningEffort::High};
    for (ReasoningEffort effort : efforts) {
        QAction *action = effortMenu->addAction(effortName(effort));
        action->setCheckable(true);
        action->setActionGroup(effortGroup);
        action->setData(static_cast<int>(effort));
        connect(action, &QAction::triggered, this, [this, effort]() {
            setReasoningEffort(effort);
        });
    }
    m_effortButton->setMenu(effortMenu);
    updateEffortButton(m_chat->reasoningEffort()); // sets the button text and the checked item

    m_sendButton = new SendButton();
    m_sendButton->setEnabled(false);

    QHBoxLayout *toolbar = new QHBoxLayout();
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(2);
    toolbar->addWidget(m_newChatButton);
    toolbar->addWidget(m_addImageButton);
    toolbar->addStretch(1);
    toolbar->addWidget(m_effortButton);
    toolbar->addSpacing(4);
    toolbar->addWidget(m_sendButton);

    QVBoxLayout *composerLayout = new QVBoxLayout(m_composer);
    composerLayout->setContentsMargins(8, 6, 8, 6);
    composerLayout->setSpacing(4);
    composerLayout->addWidget(m_input);
    composerLayout->addLayout(toolbar);

    updateComposerStyle();

    QWidget *mainWidget = new QWidget();
    QVBoxLayout *mainLayout = new QVBoxLayout(mainWidget);
    mainLayout->setContentsMargins(0, 4, 0, 12);
    mainLayout->setSpacing(8);
    mainLayout->addWidget(m_scrollArea, 1);

    // the composer has the same side margins as the message cards, so their edges line up
    QHBoxLayout *composerRow = new QHBoxLayout();
    composerRow->setContentsMargins(kSideMargin, 0, kSideMargin, 0);
    composerRow->addWidget(m_composer);
    mainLayout->addLayout(composerRow, 0);
    setWidget(mainWidget);

    connect(m_sendButton, SIGNAL(clicked()), this, SLOT(slotSendOrStop()));
    connect(m_newChatButton, SIGNAL(clicked()), this, SLOT(slotNewChat()));
    connect(m_addImageButton, &QAbstractButton::clicked, this, &Dock::addImageRequested);
    connect(m_input, SIGNAL(textChanged()), this, SLOT(slotInputChanged()));

    // the input grows with its content, from 2 up to 6 lines
    connect(m_input->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this]() {
        updateInputHeight();
    });

    updateFonts();
}

Dock::~Dock()
{
    // The model stops the worker thread and deletes the tools; they use the document access and the
    // progress window, which are still there at this point.
    delete m_chat;
    m_chat = nullptr;
    delete m_documentAccess;
}

void Dock::addUserMessage(const QString &text)
{
    appendMessageWidget(new ChatMessageWidget(true, i18n("You"), text, QDateTime::currentDateTime(), -1));
}

void Dock::addAssistantMessage(const QString &text, qint64 processingMs)
{
    appendMessageWidget(new ChatMessageWidget(false, i18n("AI"), text, QDateTime::currentDateTime(), processingMs, -1, true));
}

void Dock::addErrorMessage(const QString &text)
{
    appendMessageWidget(new ChatMessageWidget(false, i18n("Error"), text, QDateTime::currentDateTime(), -1));
}

void Dock::addStoredMessage(const StoredMessage &message)
{
    const bool isUser = (message.role == QLatin1String("user"));
    appendMessageWidget(new ChatMessageWidget(isUser, isUser ? i18n("You") : i18n("AI"), message.content, message.createdAt, message.processingMs, message.tokens, !isUser));
}

void Dock::startStreamingMessage()
{
    // the answer starts as an ellipsis and fills up while it arrives
    auto *card = new ChatMessageWidget(false, i18n("AI"), QString(QChar(0x2026)), QDateTime::currentDateTime(), -1, -1, true);
    m_streamingMessage = card;
    appendMessageWidget(card);
}

void Dock::discardStreamingMessage()
{
    if (!m_streamingMessage) {
        return;
    }

    m_messagesLayout->removeWidget(m_streamingMessage);
    m_streamingMessage->hide(); // gone from the screen at once, deleted when the event loop is idle
    m_streamingMessage->deleteLater();
    m_streamingMessage = nullptr;

    m_messageCount = qMax(0, m_messageCount - 1);
    m_newChatButton->setEnabled(m_messageCount > 0);
    if (m_messageCount == 0) {
        m_emptyHint->show();
    }
}

void Dock::clearHistory()
{
    // everything between the empty-hint (index 0) and the trailing stretch
    while (m_messagesLayout->count() > 2) {
        QLayoutItem *item = m_messagesLayout->takeAt(1);
        if (item->widget()) {
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }
    m_streamingMessage = nullptr; // it was one of the cards that are gone now
    m_emptyHint->show();
    m_followBottom = true;
    m_messageCount = 0;
    m_newChatButton->setEnabled(false);
}

bool Dock::isBusy() const
{
    return m_chat->isBusy();
}

ReasoningEffort Dock::reasoningEffort() const
{
    return m_chat->reasoningEffort();
}

void Dock::setReasoningEffort(ReasoningEffort effort)
{
    m_chat->setReasoningEffort(effort); // the button follows through reasoningEffortChanged()
}

void Dock::updateEffortButton(ReasoningEffort effort)
{
    m_effortButton->setText(i18n("Thinking: %1", effortName(effort)));
    const QList<QAction *> actions = m_effortButton->menu()->actions();
    for (QAction *action : actions) {
        if (action->data().isValid()) {
            action->setChecked(action->data().toInt() == static_cast<int>(effort));
        }
    }
}

bool Dock::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_input) {
        if (event->type() == QEvent::KeyPress) {
            QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
            const bool isEnter = keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter;
            if (isEnter && !(keyEvent->modifiers() & Qt::ShiftModifier)) {
                slotSend();
                return true;
            }
        } else if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
            // highlight the border of the whole composer while typing
            m_composer->setProperty("focused", event->type() == QEvent::FocusIn);
            m_composer->style()->unpolish(m_composer);
            m_composer->style()->polish(m_composer);
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

void Dock::slotSendOrStop()
{
    if (m_chat->isBusy()) {
        m_chat->stop();
        return;
    }
    slotSend();
}

void Dock::slotSend()
{
    const QString text = m_input->toPlainText().trimmed();

    // Nothing is sent while the AI is busy, the history is loading, the text is blank or the service is
    // not set up (the model reports that through errorReported()); the text stays in the box.
    if (!m_chat->send(text)) {
        return;
    }

    m_input->clear();
    addUserMessage(text);
    startStreamingMessage();
    Q_EMIT messageSubmitted(text);
}

void Dock::slotNewChat()
{
    clearHistory();
    m_input->clear();
    m_chat->newChat();
    Q_EMIT newChatRequested();
}

void Dock::slotInputChanged()
{
    updateSendButton();
}

void Dock::updateSendButton()
{
    // while the AI answers the button stops the answer, so it is always usable
    m_sendButton->setEnabled(m_chat->isBusy() || (m_chat->isReady() && !m_input->toPlainText().trimmed().isEmpty()));
}

void Dock::slotBusyChanged(bool busy)
{
    if (SendButton *button = dynamic_cast<SendButton *>(m_sendButton)) {
        button->setStopMode(busy);
    }
    updateSendButton();
}

void Dock::slotHistoryLoaded(const QList<StoredMessage> &messages)
{
    for (const StoredMessage &message : messages) {
        addStoredMessage(message);
    }
    updateSendButton();
}

void Dock::slotAssistantDelta(const QString &, const QString &textSoFar)
{
    ChatMessageWidget *card = dynamic_cast<ChatMessageWidget *>(m_streamingMessage);
    if (!card) {
        return; // the chat was cleared while the answer was on its way
    }
    card->setBodyText(textSoFar);
}

void Dock::slotAssistantFinished(const StoredMessage &message)
{
    ChatMessageWidget *card = dynamic_cast<ChatMessageWidget *>(m_streamingMessage);
    if (!card) {
        return;
    }
    card->setBodyText(message.content);
    card->setProcessingTime(message.processingMs);
    card->setTokens(message.tokens);
    card->setSentAt(message.createdAt);
    m_streamingMessage = nullptr;
}

void Dock::slotRequestFailed(const QString &error)
{
    // an answer that had started was finished by slotAssistantFinished(); an empty one goes away
    discardStreamingMessage();
    addErrorMessage(error);
}

void Dock::slotRequestCanceled()
{
    discardStreamingMessage();
}
void Dock::updateComposerStyle()
{
    const QPalette pal = palette();
    const QColor ink = pal.color(QPalette::WindowText);

    m_composer->setStyleSheet(
        QString("QFrame#aiChatComposer { background: %1; border: 1px solid %2; border-radius: 10px; }"
                "QFrame#aiChatComposer[focused=\"true\"] { border: 1px solid %3; }"
                "QFrame#aiChatComposer QPlainTextEdit { background: transparent; border: none; }"
                "QFrame#aiChatComposer QToolButton { background: transparent; border: none; border-radius: 6px; padding: 4px 8px; }"
                "QFrame#aiChatComposer QToolButton:hover { background: %4; }"
                "QFrame#aiChatComposer QToolButton::menu-indicator { image: none; width: 0px; }")
            .arg(pal.color(QPalette::Base).name(),
                 pal.color(QPalette::Mid).name(),
                 pal.color(QPalette::Highlight).name(),
                 rgba(ink, 30)));
}

void Dock::updateInputHeight()
{
    const QTextDocument *doc = m_input->document();
    const int lines = qBound(2, static_cast<int>(doc->size().height()), 6);
    const QFontMetrics metrics(m_input->font());
    m_input->setFixedHeight(lines * metrics.lineSpacing() + static_cast<int>(doc->documentMargin() * 2)
                            + 2 * m_input->frameWidth());
}

void Dock::appendMessageWidget(QWidget *messageWidget)
{
    m_emptyHint->hide();

    // insert before the trailing stretch
    m_messagesLayout->insertWidget(m_messagesLayout->count() - 1, messageWidget);

    ++m_messageCount;
    m_newChatButton->setEnabled(true);

    // the card was created without knowing the docker's font: apply it now
    if (ChatMessageWidget *message = dynamic_cast<ChatMessageWidget *>(messageWidget)) {
        message->applyFonts(font());
    }
}

void Dock::changeEvent(QEvent *event)
{
    QDockWidget::changeEvent(event);

    // Krita sets the docker's font from the user's interface font preference,
    // also while running when the preference changes.
    if (event->type() == QEvent::FontChange) {
        updateFonts();
    } else if (event->type() == QEvent::PaletteChange) {
        updateComposerStyle();
    }
}

void Dock::updateFonts()
{
    const QFont base = font();
    const QFont bodyFont = scaledFont(base, kBodyFontScale);

    // what you type should look like what you read
    m_input->setFont(bodyFont);
    updateInputHeight();

    // messages sit between the empty-hint (index 0) and the trailing stretch
    for (int i = 1; i < m_messagesLayout->count() - 1; ++i) {
        QLayoutItem *item = m_messagesLayout->itemAt(i);
        if (ChatMessageWidget *message = dynamic_cast<ChatMessageWidget *>(item->widget())) {
            message->applyFonts(base);
        }
    }
}

} // namespace AiChat

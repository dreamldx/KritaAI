/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AiChatDock.h"

#include <QAbstractButton>
#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QActionGroup>
#include <QDateTime>
#include <QDesktopServices>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPolygonF>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTextDocument>
#include <QThread>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <klocalizedstring.h>

#include "AiChatConfig.h"
#include "AiChatDocumentTools.h"
#include "AiChatTools.h"
#include "AiChatVision.h"
#include "AiChatWorker.h"

namespace {

/// Blend two colors; ratio is the share of `a` (0.0 .. 1.0).
QColor blend(const QColor &a, const QColor &b, qreal ratio)
{
    return QColor::fromRgbF(a.redF() * ratio + b.redF() * (1.0 - ratio),
                            a.greenF() * ratio + b.greenF() * (1.0 - ratio),
                            a.blueF() * ratio + b.blueF() * (1.0 - ratio));
}

QString rgba(const QColor &c, int alpha)
{
    return QString("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
}

/// "820 ms" below one second, "1.4 s" above.
/// How long the AI needed, always in seconds: "0.4 s", "12.3 s".
QString formatDuration(qint64 ms)
{
    return i18nc("duration in seconds", "%1 s", QString::number(ms / 1000.0, 'f', 1));
}

/**
 * Qt only wraps a label at spaces. Give very long unbroken runs (URLs,
 * paths) a place to wrap, otherwise they would be cut off.
 */
QString allowWrapInLongWords(const QString &text)
{
    static const QRegularExpression longRun("(\\S{28})(?=\\S)");
    QString result = text;
    result.replace(longRun, QString("\\1") + QChar(0x200B));
    return result;
}

// Font sizes, relative to the docker's font (which follows Krita's interface font).
const qreal kBodyFontScale = 1.2;
const qreal kMetaFontScale = 0.9;

// Space between the panel's left/right edge and the message cards and the input box.
const int kSideMargin = 16;

/// A copy of `base` scaled by `ratio`; works for point- and pixel-sized fonts.
QFont scaledFont(const QFont &base, qreal ratio)
{
    QFont font = base;
    if (base.pixelSize() > 0) {
        font.setPixelSize(qMax(1, qRound(base.pixelSize() * ratio)));
    } else {
        font.setPointSizeF(base.pointSizeF() * ratio);
    }
    return font;
}

/// The round icon at the start of a message header: a person or a sparkle.
class AvatarWidget : public QWidget
{
public:
    AvatarWidget(bool isUser, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_isUser(isUser)
    {
        setFixedSize(26, 26);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QPalette pal = palette();
        const QColor fill = m_isUser ? pal.color(QPalette::Highlight) : QColor(124, 92, 214);
        const QColor glyph = m_isUser ? pal.color(QPalette::HighlightedText) : QColor(Qt::white);

        const QRectF circle = QRectF(rect()).adjusted(1, 1, -1, -1);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawEllipse(circle);

        p.setBrush(glyph);
        const QPointF c = circle.center();

        if (m_isUser) {
            // head and shoulders, clipped to the circle
            QPainterPath clip;
            clip.addEllipse(circle);
            p.setClipPath(clip);
            p.drawEllipse(QPointF(c.x(), c.y() - 3.2), 3.6, 3.6);
            p.drawEllipse(QRectF(c.x() - 6.5, c.y() + 2.2, 13.0, 11.0));
        } else {
            // four-pointed sparkle
            const qreal r = 6.6;
            QPainterPath star;
            star.moveTo(c.x(), c.y() - r);
            star.quadTo(c, QPointF(c.x() + r, c.y()));
            star.quadTo(c, QPointF(c.x(), c.y() + r));
            star.quadTo(c, QPointF(c.x() - r, c.y()));
            star.quadTo(c, QPointF(c.x(), c.y() - r));
            p.drawPath(star);
        }
    }

private:
    bool m_isUser;
};

/// A label that may be narrower than its longest word, so it can never force a horizontal scrollbar.
/**
 * The same for the markdown of an answer of the AI: a long run gets places to wrap, but not where
 * that would break the markdown. These stay as they are:
 *  - code (inline `code` and fenced blocks, which cannot wrap anyway),
 *  - the target of a link, and
 *  - a link that is written out in full: [text](https://...) and <https://...>.
 * A bare web address is turned into a link whose text can wrap and whose target is intact:
 * https://very.long/address  ->  [https://very.long/ad<ZWSP>dress](https://very.long/address)
 * Raw HTML (<b>, <a href=...>, <img ...>) is made harmless: it is shown as the text it is.
 */
QString markdownWithWrapPoints(const QString &markdown)
{
    static const int kRun = 28;
    // 1 inline code, 2 a link or image with its target, 3 <autolink> (a web or mail address),
    // 4 a bare address, 5 the "<" of raw HTML, 6 a long run
    static const QRegularExpression token("(`+[^`\\n]*`+)"
                                          "|(!?\\[[^\\]\\n]*\\]\\([^)\\n]*\\))"
                                          "|(<(?:[A-Za-z][A-Za-z0-9+.\\-]*:[^>\\s]*|[^>\\s@]+@[^>\\s]+)>)"
                                          "|(https?://[^\\s<>]+)"
                                          "|(<(?=[A-Za-z/!?]))"
                                          "|(\\S{29,})");

    auto breakable = [](const QString &run) {
        QString result;
        for (int i = 0; i < run.size(); i += kRun) {
            if (i > 0) {
                result += QChar(0x200B);
            }
            result += run.mid(i, kRun);
        }
        return result;
    };

    // "<b>" and "</a>" become text. A "<" that starts no tag (a < b, <3) is not touched.
    auto withoutHtml = [](QString text) {
        static const QRegularExpression htmlStart("<(?=[A-Za-z/!?])");
        return text.replace(htmlStart, QStringLiteral("&lt;"));
    };

    QStringList lines = markdown.split(QLatin1Char('\n'));
    bool inFence = false;
    for (QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QLatin1String("```")) || trimmed.startsWith(QLatin1String("~~~"))) {
            inFence = !inFence;
            continue;
        }
        if (inFence) {
            continue;
        }

        QString result;
        int last = 0;
        QRegularExpressionMatchIterator it = token.globalMatch(line);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            result += line.mid(last, m.capturedStart() - last);
            last = m.capturedEnd();

            if (m.capturedLength(4) > 0) {
                // the punctuation that ends a sentence is not part of the address
                QString address = m.captured(4);
                QString tail;
                while (!address.isEmpty() && QStringLiteral(".,;:!?)'\"").contains(address.at(address.size() - 1))) {
                    tail.prepend(address.at(address.size() - 1));
                    address.chop(1);
                }
                result += QStringLiteral("[%1](%2)%3").arg(address.size() > kRun ? breakable(address) : address, address, tail);
            } else if (m.capturedLength(5) > 0) {
                // Raw HTML is not allowed: Qt would turn <a href="file:///..."> into a real link. Shown as text.
                result += QStringLiteral("&lt;");
            } else if (m.capturedLength(6) > 0) {
                // a long run can hold tags too: "href=...>here</a>"
                result += breakable(withoutHtml(m.captured(6)));
            } else if (m.capturedLength(2) > 0) {
                // a link: its text may hold raw HTML, its target stays as it is
                const QString link = m.captured(2);
                const int split = link.indexOf(QLatin1String("]("));
                result += split < 0 ? link : withoutHtml(link.left(split)) + link.mid(split);
            } else {
                result += m.captured(0); // code and autolinks stay as they are
            }
        }
        result += line.mid(last);
        line = result;
    }
    return lines.join(QLatin1Char('\n'));
}

class BodyLabel : public QLabel
{
public:
    using QLabel::QLabel;

    QSize minimumSizeHint() const override
    {
        return QSize(0, QLabel::minimumSizeHint().height());
    }
};

/// The round send button with an arrow pointing up.
class SendButton : public QAbstractButton
{
public:
    explicit SendButton(QWidget *parent = nullptr)
        : QAbstractButton(parent)
    {
        setFixedSize(30, 30);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setAttribute(Qt::WA_Hover);
        setToolTip(i18n("Send (Enter)"));
    }

    /// While the AI answers, the button shows a square and stops the answer when clicked.
    void setStopMode(bool stop)
    {
        if (m_stop == stop) {
            return;
        }
        m_stop = stop;
        setToolTip(stop ? i18n("Stop") : i18n("Send (Enter)"));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QPalette pal = palette();
        QColor fill;
        QColor arrow;
        if (isEnabled()) {
            fill = pal.color(QPalette::Highlight);
            if (isDown()) {
                fill = fill.darker(125);
            } else if (underMouse()) {
                fill = fill.lighter(115);
            }
            arrow = pal.color(QPalette::HighlightedText);
        } else {
            fill = blend(pal.color(QPalette::WindowText), pal.color(QPalette::Window), 0.16);
            arrow = blend(pal.color(QPalette::WindowText), pal.color(QPalette::Window), 0.4);
        }

        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawEllipse(QRectF(rect()).adjusted(1, 1, -1, -1));

        const QPointF c = QRectF(rect()).center();

        if (m_stop) {
            p.setPen(Qt::NoPen);
            p.setBrush(arrow);
            p.drawRoundedRect(QRectF(c.x() - 5.0, c.y() - 5.0, 10.0, 10.0), 2.0, 2.0);
            return;
        }

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(arrow, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(QPointF(c.x(), c.y() + 6.0), QPointF(c.x(), c.y() - 6.0));
        QPolygonF head;
        head << QPointF(c.x() - 5.5, c.y() - 0.5) << QPointF(c.x(), c.y() - 6.0) << QPointF(c.x() + 5.5, c.y() - 0.5);
        p.drawPolyline(head);
    }

private:
    bool m_stop {false};
};

/// A small flat button that draws a vector icon in the text color.
class IconButton : public QAbstractButton
{
public:
    enum Glyph {
        NewChat,
        Image,
    };

    IconButton(Glyph glyph, const QString &toolTip, QWidget *parent = nullptr)
        : QAbstractButton(parent)
        , m_glyph(glyph)
    {
        setFixedSize(28, 28);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setAttribute(Qt::WA_Hover);
        setToolTip(toolTip);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QPalette pal = palette();
        const QColor ink = isEnabled() ? pal.color(QPalette::WindowText)
                                       : blend(pal.color(QPalette::WindowText), pal.color(QPalette::Window), 0.35);

        if (isEnabled() && (underMouse() || isDown())) {
            QColor hover = ink;
            hover.setAlpha(isDown() ? 55 : 32);
            p.setPen(Qt::NoPen);
            p.setBrush(hover);
            p.drawRoundedRect(QRectF(rect()).adjusted(0, 0, -1, -1), 6, 6);
        }

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPointF c = QRectF(rect()).center();

        if (m_glyph == NewChat) {
            p.drawLine(QPointF(c.x() - 6.0, c.y()), QPointF(c.x() + 6.0, c.y()));
            p.drawLine(QPointF(c.x(), c.y() - 6.0), QPointF(c.x(), c.y() + 6.0));
        } else {
            p.drawRoundedRect(QRectF(c.x() - 8.0, c.y() - 6.5, 16.0, 13.0), 2.5, 2.5);
            p.setBrush(ink);
            p.drawEllipse(QPointF(c.x() - 3.5, c.y() - 2.0), 1.5, 1.5);
            p.setBrush(Qt::NoBrush);
            QPolygonF mountains;
            mountains << QPointF(c.x() - 6.5, c.y() + 4.5) << QPointF(c.x() - 1.5, c.y() - 0.5)
                      << QPointF(c.x() + 2.0, c.y() + 3.0) << QPointF(c.x() + 4.0, c.y() + 1.0)
                      << QPointF(c.x() + 6.5, c.y() + 4.5);
            p.drawPolyline(mountains);
        }
    }

private:
    Glyph m_glyph;
};

QString effortName(AiChatDock::ReasoningEffort effort)
{
    switch (effort) {
    case AiChatDock::Low:
        return i18n("Low");
    case AiChatDock::High:
        return i18n("High");
    case AiChatDock::Medium:
    default:
        return i18n("Medium");
    }
}

/// One message: header (icon | name), body, footer. The footer of a message of the user (and of an
/// error) shows the time it was sent. The footer of an answer of the AI shows no clock time but
/// what the answer cost: the tokens and the seconds the AI needed.
class ChatMessageWidget : public QFrame
{
public:
    ChatMessageWidget(bool isUser,
                      const QString &name,
                      const QString &text,
                      const QDateTime &sentAt,
                      qint64 processingMs,
                      qint64 tokens = -1,
                      bool isAnswer = false,
                      QWidget *parent = nullptr)
        : QFrame(parent)
        , m_isAnswer(isAnswer)
    {
        setObjectName("aiChatMessage");

        const QPalette pal = palette();
        const QColor dim = blend(pal.color(QPalette::WindowText), pal.color(QPalette::Window), 0.6);

        // A card with a fill and a thin border. The AI card's fill is derived from the text color,
        // so it is visible in every theme; the user's card is tinted with the highlight color.
        const QString background = isUser ? rgba(pal.color(QPalette::Highlight), 40)
                                          : blend(pal.color(QPalette::WindowText), pal.color(QPalette::Window), 0.07).name();
        const QString border = isUser ? rgba(pal.color(QPalette::Highlight), 110) : pal.color(QPalette::Mid).name();

        setStyleSheet(QString("QFrame#aiChatMessage { background: %1; border: 1px solid %2; border-radius: 8px; }"
                              "QFrame#aiChatMessage QLabel { background: transparent; border: none; }"
                              "QLabel#aiChatDim { color: %3; }")
                          .arg(background, border, dim.name()));

        QVBoxLayout *layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 8, 10, 6);
        layout->setSpacing(5);

        // header: icon | name
        m_headerLayout = new QHBoxLayout();
        m_headerLayout->setSpacing(7);
        m_headerLayout->addWidget(new AvatarWidget(isUser));

        m_name = new QLabel(name);
        m_headerLayout->addWidget(m_name);
        m_headerLayout->addStretch(1);
        layout->addLayout(m_headerLayout);
        layout->addSpacing(6); // extra air between the header and the body

        // body: the answers of the AI are markdown, everything else is plain text
        m_body = new BodyLabel();
        m_body->setTextFormat(isAnswer ? Qt::MarkdownText : Qt::PlainText);
        setBodyText(text);
        m_body->setWordWrap(true);
        m_body->setCursor(Qt::IBeamCursor);
        if (isAnswer) {
            m_body->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
            // Links are not opened by Qt itself: only web and mail addresses are, whatever the AI wrote
            // (a markdown link may point to file:// or to a program), and the address is shown on hover.
            m_body->setOpenExternalLinks(false);
            QObject::connect(m_body, &QLabel::linkActivated, m_body, [](const QString &link) {
                const QUrl url(link);
                const QString scheme = url.scheme().toLower();
                if (url.isValid() && (scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto"))) {
                    QDesktopServices::openUrl(url);
                }
            });
            QObject::connect(m_body, &QLabel::linkHovered, m_body, [this](const QString &link) {
                m_body->setToolTip(link);
            });
        } else {
            m_body->setTextInteractionFlags(Qt::TextSelectableByMouse);
        }
        layout->addWidget(m_body);

        // footer, right-aligned. Answer of the AI: [tokens] seconds. Anything else: the time it was sent.
        m_footerLayout = new QHBoxLayout();
        m_footerLayout->setSpacing(8);
        m_footerLayout->addStretch(1);

        m_time = new QLabel(isAnswer ? QString() : sentAt.toString("HH:mm:ss"));
        m_time->setObjectName("aiChatDim");
        if (!isAnswer) {
            m_time->setToolTip(sentAt.toString(Qt::TextDate));
        }
        m_footerLayout->addWidget(m_time);
        layout->addLayout(m_footerLayout);

        applyFonts(font());

        if (isAnswer) {
            setProcessingTime(processingMs);
            setTokens(tokens); // only if the server said it
        }
    }

    /// Show how many tokens the answer cost, left of the time. Does nothing for a negative value.
    void setTokens(qint64 tokens)
    {
        if (tokens < 0) {
            return;
        }

        if (!m_tokens) {
            m_tokens = new QLabel();
            m_tokens->setObjectName("aiChatDim");
            m_tokens->setToolTip(i18n("Tokens used by this answer: everything that was sent to the AI and everything it "
                                      "wrote, thinking included, added up over all the requests the answer needed"));
            m_footerLayout->insertWidget(m_footerLayout->count() - 1, m_tokens); // just before the time
            applyFonts(m_baseFont);
        }
        m_tokens->setText(i18n("%1 tokens", QLocale().toString(tokens)));
    }

    /// Replace the text of the message (used while an answer is streaming in).
    void setBodyText(const QString &text)
    {
        m_rawText = text;
        // QLabel::text() keeps the markdown as it was written; the label shows it formatted
        m_body->setText(m_isAnswer ? markdownWithWrapPoints(text) : allowWrapInLongWords(text));
    }

    QString bodyText() const { return m_rawText; }

    /// Answers of the AI: show how many seconds the AI needed, at the right end of the footer.
    /// Does nothing for a negative value or for a card that is not an answer.
    void setProcessingTime(qint64 ms)
    {
        if (ms < 0 || !m_isAnswer) {
            return;
        }
        m_time->setText(formatDuration(ms));
        m_time->setToolTip(i18n("Time the AI needed to answer"));
    }

    /// The send time is shown on every card except the answers of the AI: they show no clock time.
    void setSentAt(const QDateTime &sentAt)
    {
        if (m_isAnswer) {
            return;
        }
        m_time->setText(sentAt.toString("HH:mm:ss"));
        m_time->setToolTip(sentAt.toString(Qt::TextDate));
    }

    /// Set every font from `base` (the docker's font); the body is larger than the rest.
    void applyFonts(const QFont &base)
    {
        m_baseFont = base;

        QFont nameFont = base;
        nameFont.setBold(true);
        m_name->setFont(nameFont);

        const QFont metaFont = scaledFont(base, kMetaFontScale);
        m_time->setFont(metaFont);
        if (m_tokens) {
            m_tokens->setFont(metaFont);
        }

        m_body->setFont(scaledFont(base, kBodyFontScale));
    }

private:
    QHBoxLayout *m_headerLayout {nullptr};
    QLabel *m_name {nullptr};
    bool m_isAnswer {false};
    QHBoxLayout *m_footerLayout {nullptr};
    QLabel *m_tokens {nullptr};
    QLabel *m_time {nullptr};
    BodyLabel *m_body {nullptr};
    QString m_rawText;
    QFont m_baseFont;
};

} // namespace

AiChatDock::AiChatDock(const QString &databasePath)
    : QDockWidget()
    , m_scrollArea(new QScrollArea())
    , m_messagesContainer(new QWidget())
    , m_messagesLayout(new QVBoxLayout(m_messagesContainer))
    , m_emptyHint(new QLabel(i18n("No messages yet.\nType below to start a conversation.")))
    , m_composer(new QFrame())
    , m_input(new QPlainTextEdit())
{
    setWindowTitle(i18n("AI Chat"));

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
    const ReasoningEffort efforts[] = {Low, Medium, High};
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
    setReasoningEffort(m_effort); // sets the button text and the checked item

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
    connect(m_addImageButton, &QAbstractButton::clicked, this, &AiChatDock::addImageRequested);
    connect(m_input, SIGNAL(textChanged()), this, SLOT(slotInputChanged()));

    // the input grows with its content, from 2 up to 6 lines
    connect(m_input->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this]() {
        updateInputHeight();
    });

    updateFonts();

    // The AI loop, the network and the database run in a thread of their own. They are reached
    // only through queued signals and slots, so the panel never waits for any of them.
    qRegisterMetaType<AiChatStoredMessage>();
    qRegisterMetaType<QList<AiChatStoredMessage>>();
    qRegisterMetaType<AiChatRequestSettings>();

    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("AiChatWorker"));
    m_worker = new AiChatWorker(databasePath.isEmpty() ? AiChatWorker::defaultDatabasePath() : databasePath);

    // The tools. The executor lives here, in the GUI thread: the tools call it from the worker thread.
    m_executor = new AiChatGuiExecutor(this);
    m_documentAccess = new KritaDocumentAccess();
    m_vision = new AiChatVisionClient(); // used by the worker thread only
    m_tools = new AiChatToolRegistry();
    aiChatAddDocumentTools(m_tools, m_executor, m_documentAccess, m_vision);
    m_worker->setToolProvider(m_tools);

    m_worker->moveToThread(m_thread);

    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_thread, &QThread::started, m_worker, &AiChatWorker::init);
    connect(m_worker, &AiChatWorker::conversationLoaded, this, &AiChatDock::slotConversationLoaded);
    connect(m_worker, &AiChatWorker::assistantDelta, this, &AiChatDock::slotAssistantDelta);
    connect(m_worker, &AiChatWorker::assistantFinished, this, &AiChatDock::slotAssistantFinished);
    connect(m_worker, &AiChatWorker::requestFailed, this, &AiChatDock::slotRequestFailed);
    connect(m_worker, &AiChatWorker::requestCanceled, this, &AiChatDock::slotRequestCanceled);
    connect(m_worker, &AiChatWorker::busyChanged, this, &AiChatDock::slotBusyChanged);
    connect(m_worker, &AiChatWorker::storageError, this, &AiChatDock::slotStorageError);

    m_thread->start();
}

AiChatDock::~AiChatDock()
{
    // A tool that waits for the GUI thread would block the shutdown below: let it go first.
    m_executor->close();

    if (m_thread->isRunning()) {
        // The worker stops a running request and releases its network and database
        // connections inside its own thread; only then the thread may end.
        QMetaObject::invokeMethod(m_worker, "shutdown", Qt::BlockingQueuedConnection);
        m_thread->quit();
        if (!m_thread->wait(5000)) {
            qWarning("AI Chat: the worker thread did not stop in time");
        }
    }
    // the worker deletes itself when the thread has finished

    // The tools are used by the worker thread only, which has ended (or, after the warning above,
    // is stuck and must not be touched: then they are left alone and leak rather than crash).
    if (!m_thread->isRunning()) {
        delete m_tools;
        delete m_vision;
        delete m_documentAccess;
    }
}

void AiChatDock::addUserMessage(const QString &text)
{
    appendMessageWidget(new ChatMessageWidget(true, i18n("You"), text, QDateTime::currentDateTime(), -1));
}

void AiChatDock::addAssistantMessage(const QString &text, qint64 processingMs)
{
    appendMessageWidget(new ChatMessageWidget(false, i18n("AI"), text, QDateTime::currentDateTime(), processingMs, -1, true));
}

void AiChatDock::addErrorMessage(const QString &text)
{
    appendMessageWidget(new ChatMessageWidget(false, i18n("Error"), text, QDateTime::currentDateTime(), -1));
}

void AiChatDock::addStoredMessage(const AiChatStoredMessage &message)
{
    const bool isUser = (message.role == QLatin1String("user"));
    appendMessageWidget(new ChatMessageWidget(isUser, isUser ? i18n("You") : i18n("AI"), message.content, message.createdAt, message.processingMs, message.tokens, !isUser));
}

void AiChatDock::startStreamingMessage()
{
    // the answer starts as an ellipsis and fills up while it arrives
    auto *card = new ChatMessageWidget(false, i18n("AI"), QString(QChar(0x2026)), QDateTime::currentDateTime(), -1, -1, true);
    m_streamingMessage = card;
    m_streamingText.clear();
    appendMessageWidget(card);
}

void AiChatDock::discardStreamingMessage()
{
    if (!m_streamingMessage) {
        return;
    }

    m_messagesLayout->removeWidget(m_streamingMessage);
    m_streamingMessage->hide(); // gone from the screen at once, deleted when the event loop is idle
    m_streamingMessage->deleteLater();
    m_streamingMessage = nullptr;
    m_streamingText.clear();

    m_messageCount = qMax(0, m_messageCount - 1);
    m_newChatButton->setEnabled(m_messageCount > 0);
    if (m_messageCount == 0) {
        m_emptyHint->show();
    }
}

void AiChatDock::clearHistory()
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
    m_streamingText.clear();
    m_emptyHint->show();
    m_followBottom = true;
    m_messageCount = 0;
    m_newChatButton->setEnabled(false);
}

AiChatDock::ReasoningEffort AiChatDock::reasoningEffort() const
{
    return m_effort;
}

void AiChatDock::setReasoningEffort(ReasoningEffort effort)
{
    const bool changed = (effort != m_effort);
    m_effort = effort;

    m_effortButton->setText(i18n("Thinking: %1", effortName(effort)));
    const QList<QAction *> actions = m_effortButton->menu()->actions();
    for (QAction *action : actions) {
        if (action->data().isValid()) {
            action->setChecked(action->data().toInt() == static_cast<int>(effort));
        }
    }

    if (changed) {
        Q_EMIT reasoningEffortChanged(effort);
    }
}

bool AiChatDock::eventFilter(QObject *watched, QEvent *event)
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

void AiChatDock::slotSendOrStop()
{
    if (m_busy) {
        QMetaObject::invokeMethod(m_worker, "cancel", Qt::QueuedConnection);
        return;
    }
    slotSend();
}

void AiChatDock::slotSend()
{
    // One request at a time; and nothing can be sent before the history is loaded.
    if (m_busy || !m_ready) {
        return;
    }

    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty()) {
        return;
    }

    // The settings are read now, so a change in Configure Krita counts from the next message on.
    const AiChatConfig config = AiChatConfig::load();
    if (!config.isComplete()) {
        // the text stays in the box, nothing is lost
        addErrorMessage(i18n("The AI service is not set up yet. Open Settings > Configure Krita > AI Chat "
                             "and enter the server address and the model."));
        return;
    }

    AiChatRequestSettings request;
    request.url = config.chatCompletionsUrl();
    request.apiKey = config.apiKey;
    request.model = config.model;
    if (config.hasVision()) {
        request.vision.url = AiChatConfig::chatCompletionsUrl(config.visionBaseUrl);
        request.vision.apiKey = config.visionApiKey;
        request.vision.model = config.visionModel.trimmed();
        request.vision.disableThinking = config.visionDisableThinking;
    }
    if (config.sendReasoningEffort) {
        switch (m_effort) {
        case Low:
            request.reasoningEffort = QStringLiteral("low");
            break;
        case High:
            request.reasoningEffort = QStringLiteral("high");
            break;
        case Medium:
        default:
            request.reasoningEffort = QStringLiteral("medium");
            break;
        }
    }

    m_input->clear();
    addUserMessage(text);
    startStreamingMessage();
    setBusy(true); // at once; the worker confirms it with busyChanged()

    QMetaObject::invokeMethod(m_worker, "submit", Qt::QueuedConnection,
                              Q_ARG(QString, text), Q_ARG(AiChatRequestSettings, request));
    Q_EMIT messageSubmitted(text);
}

void AiChatDock::slotNewChat()
{
    clearHistory();
    m_input->clear();
    // a request that is still running is stopped by the worker; the old conversation stays in the database
    QMetaObject::invokeMethod(m_worker, "startNewConversation", Qt::QueuedConnection);
    Q_EMIT newChatRequested();
}

void AiChatDock::slotInputChanged()
{
    updateSendButton();
}

void AiChatDock::updateSendButton()
{
    // while the AI answers the button stops the answer, so it is always usable
    m_sendButton->setEnabled(m_busy || (m_ready && !m_input->toPlainText().trimmed().isEmpty()));
}

void AiChatDock::setBusy(bool busy)
{
    m_busy = busy;
    if (SendButton *button = dynamic_cast<SendButton *>(m_sendButton)) {
        button->setStopMode(busy);
    }
    updateSendButton();
}

void AiChatDock::slotConversationLoaded(const QList<AiChatStoredMessage> &messages)
{
    for (const AiChatStoredMessage &message : messages) {
        addStoredMessage(message);
    }
    m_ready = true;
    updateSendButton();
}

void AiChatDock::slotAssistantDelta(const QString &delta)
{
    ChatMessageWidget *card = dynamic_cast<ChatMessageWidget *>(m_streamingMessage);
    if (!card) {
        return; // the chat was cleared while the answer was on its way
    }
    m_streamingText += delta;
    card->setBodyText(m_streamingText);
}

void AiChatDock::slotAssistantFinished(const AiChatStoredMessage &message)
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
    m_streamingText.clear();
}

void AiChatDock::slotRequestFailed(const QString &error)
{
    // an answer that had started was finished by slotAssistantFinished(); an empty one goes away
    discardStreamingMessage();
    addErrorMessage(error);
}

void AiChatDock::slotRequestCanceled()
{
    discardStreamingMessage();
}

void AiChatDock::slotBusyChanged(bool busy)
{
    setBusy(busy);
}

void AiChatDock::slotStorageError(const QString &error)
{
    // once is enough: the chat keeps working, it just cannot save
    if (m_storageWarned) {
        return;
    }
    m_storageWarned = true;
    addErrorMessage(i18n("The chat history cannot be saved: %1", error));
}

void AiChatDock::updateComposerStyle()
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

void AiChatDock::updateInputHeight()
{
    const QTextDocument *doc = m_input->document();
    const int lines = qBound(2, static_cast<int>(doc->size().height()), 6);
    const QFontMetrics metrics(m_input->font());
    m_input->setFixedHeight(lines * metrics.lineSpacing() + static_cast<int>(doc->documentMargin() * 2)
                            + 2 * m_input->frameWidth());
}

void AiChatDock::appendMessageWidget(QWidget *messageWidget)
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

void AiChatDock::changeEvent(QEvent *event)
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

void AiChatDock::updateFonts()
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

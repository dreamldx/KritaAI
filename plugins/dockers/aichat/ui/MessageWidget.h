/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _UI_MESSAGE_WIDGET_H_
#define _UI_MESSAGE_WIDGET_H_

// The widgets of the chat panel: the message card and the round buttons. Moved out of Dock.cpp as they
// were; the definitions are still in this header.

#include <QAbstractButton>
#include <QDateTime>
#include <QDesktopServices>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPolygonF>
#include <QRegularExpression>
#include <QStyle>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <klocalizedstring.h>

namespace AiChat {

/// Blend two colors; ratio is the share of `a` (0.0 .. 1.0).
inline QColor blend(const QColor &a, const QColor &b, qreal ratio)
{
    return QColor::fromRgbF(a.redF() * ratio + b.redF() * (1.0 - ratio),
                            a.greenF() * ratio + b.greenF() * (1.0 - ratio),
                            a.blueF() * ratio + b.blueF() * (1.0 - ratio));
}

inline QString rgba(const QColor &c, int alpha)
{
    return QString("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
}

/// "820 ms" below one second, "1.4 s" above.
/// How long the AI needed, always in seconds: "0.4 s", "12.3 s".
inline QString formatDuration(qint64 ms)
{
    return i18nc("duration in seconds", "%1 s", QString::number(ms / 1000.0, 'f', 1));
}

/**
 * Qt only wraps a label at spaces. Give very long unbroken runs (URLs,
 * paths) a place to wrap, otherwise they would be cut off.
 */
inline QString allowWrapInLongWords(const QString &text)
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
inline QFont scaledFont(const QFont &base, qreal ratio)
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
inline QString markdownWithWrapPoints(const QString &markdown)
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


} // namespace AiChat

#endif

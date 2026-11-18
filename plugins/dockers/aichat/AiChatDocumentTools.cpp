/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AiChatDocumentTools.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QPainter>

#include <KoColorProfile.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisView.h>
#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_node.h>
#include <kis_selection.h>
#include <klocalizedstring.h>

#include "AiChatLog.h"
#include "AiChatTools.h"
#include "AiChatVision.h"

namespace {

const int kMaxLayersListed = 200;

QString layerKind(const KisNodeSP &node)
{
    QString kind = QString::fromLatin1(node->metaObject()->className());
    if (kind.startsWith(QLatin1String("Kis"))) {
        kind.remove(0, 3);
    }
    return kind; // "PaintLayer", "GroupLayer", "AdjustmentLayer", ...
}

void collectLayers(const KisNodeSP &parent, int depth, QJsonArray *out, int *count, bool *truncated)
{
    // top of the stack first, like the layers docker shows them
    for (KisNodeSP node = parent->lastChild(); node; node = node->prevSibling()) {
        if (*count >= kMaxLayersListed) {
            *truncated = true;
            return;
        }
        ++(*count);

        QJsonObject entry;
        entry["name"] = node->name();
        entry["type"] = layerKind(node);
        entry["depth"] = depth;
        entry["visible"] = node->visible();
        entry["opacity_percent"] = qRound(node->opacity() * 100.0 / 255.0);
        entry["locked"] = node->userLocked();
        out->append(entry);

        if (node->childCount() > 0) {
            collectLayers(node, depth + 1, out, count, truncated);
        }
    }
}

KisImageSP activeImage(QString *error)
{
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    KisView *view = window ? window->activeView() : nullptr;
    KisImageSP image;
    if (view) {
        image = view->image(); // a weak pointer becomes a shared one
    }
    if (!image) {
        *error = i18n("There is no open document.");
    }
    return image;
}

QString jsonText(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

/// The region argument of ask_about_image: {"x":..,"y":..,"width":..,"height":..}
bool parseRegion(const QJsonValue &value, QRect *region, QString *error)
{
    if (value.isUndefined() || value.isNull()) {
        *region = QRect();
        return true;
    }
    if (!value.isObject()) {
        *error = i18n("\"region\" has to be an object with x, y, width and height.");
        return false;
    }
    const QJsonObject object = value.toObject();
    for (const char *key : {"x", "y", "width", "height"}) {
        if (!object.value(QLatin1String(key)).isDouble()) {
            *error = i18n("\"region\" has to be an object with the numbers x, y, width and height.");
            return false;
        }
    }
    *region = QRect(object.value("x").toInt(), object.value("y").toInt(), object.value("width").toInt(), object.value("height").toInt());
    if (region->width() <= 0 || region->height() <= 0) {
        *error = i18n("The width and the height of \"region\" have to be positive.");
        return false;
    }
    return true;
}

/// Shrinks to `maxSide` and puts the picture on white, so transparent areas do not turn black.
QImage prepareForVision(const QImage &source, int maxSide)
{
    QImage scaled = source;
    if (qMax(scaled.width(), scaled.height()) > maxSide) {
        scaled = scaled.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    QImage flat(scaled.size(), QImage::Format_RGB32);
    flat.fill(Qt::white);
    QPainter painter(&flat);
    painter.drawImage(0, 0, scaled);
    painter.end();
    return flat;
}

} // namespace

// ---- Krita ------------------------------------------------------------------

bool KritaDocumentAccess::documentInfo(QJsonObject *info, QString *error)
{
    KisImageSP image = activeImage(error);
    if (!image) {
        return false;
    }

    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    KisDocument *document = window->activeView()->document();

    QJsonObject result;
    result["name"] = document ? document->caption() : QString();
    result["file"] = document ? document->localFilePath() : QString();
    result["width"] = image->width();
    result["height"] = image->height();
    result["dpi"] = qRound(image->xRes() * 72.0);

    const KoColorSpace *colorSpace = image->colorSpace();
    result["color_model"] = colorSpace->colorModelId().id();
    result["color_depth"] = colorSpace->colorDepthId().id();
    result["color_profile"] = colorSpace->profile() ? colorSpace->profile()->name() : QString();

    KisSelectionSP selection = image->globalSelection();
    const QRect selected = selection ? selection->selectedExactRect() : QRect();
    if (selected.isValid() && !selected.isEmpty()) {
        QJsonObject rect;
        rect["x"] = selected.x();
        rect["y"] = selected.y();
        rect["width"] = selected.width();
        rect["height"] = selected.height();
        result["selection"] = rect;
    } else {
        result["selection"] = QJsonValue(QJsonValue::Null);
    }

    QJsonArray layers;
    int count = 0;
    bool truncated = false;
    if (image->rootLayer()) {
        collectLayers(image->rootLayer(), 0, &layers, &count, &truncated);
    }
    result["layers"] = layers;
    if (truncated) {
        result["layers_truncated"] = true;
    }

    *info = result;
    return true;
}

bool KritaDocumentAccess::renderImage(const QRect &region, bool selectionOnly, QImage *image, QRect *usedRegion, QString *error)
{
    KisImageSP kisImage = activeImage(error);
    if (!kisImage) {
        return false;
    }

    QRect wanted = kisImage->bounds();
    if (selectionOnly) {
        KisSelectionSP selection = kisImage->globalSelection();
        const QRect selected = selection ? selection->selectedExactRect() : QRect();
        if (selected.isEmpty()) {
            *error = i18n("There is no selection.");
            return false;
        }
        wanted &= selected;
    } else if (!region.isNull()) {
        wanted &= region;
    }
    if (wanted.isEmpty()) {
        *error = i18n("The region is outside of the image, which is %1 x %2 pixels.", kisImage->width(), kisImage->height());
        return false;
    }

    const KoColorProfile *profile = KoColorSpaceRegistry::instance()->rgb8()->profile();
    *image = kisImage->convertToQImage(wanted, profile);
    if (image->isNull()) {
        *error = i18n("The image could not be rendered.");
        return false;
    }
    *usedRegion = wanted;
    return true;
}

// ---- the tools --------------------------------------------------------------

void aiChatAddDocumentTools(AiChatToolRegistry *registry, AiChatGuiExecutor *executor, AiChatDocumentAccess *access,
                            AiChatVisionClient *vision)
{
    // ---- get_document_info
    AiChatTool info;
    info.name = QStringLiteral("get_document_info");
    info.description = QStringLiteral(
        "Describes the document that is open in Krita: name, size in pixels, resolution, color model and "
        "depth, the layers (top first, with type, visibility and opacity) and the bounds of the current "
        "selection (null if nothing is selected). Read-only. Use it to find out what you are working on.");
    info.run = [executor, access](const QJsonObject &, const AiChatRequestSettings &, bool *ok) -> QString {
        QJsonObject document;
        QString error;
        bool success = false;
        if (!executor->run([&]() { success = access->documentInfo(&document, &error); }, &error)) {
            *ok = false;
            return error;
        }
        *ok = success;
        return success ? jsonText(document) : error;
    };
    registry->add(info);

    // ---- ask_about_image
    QJsonObject regionProperties;
    for (const char *key : {"x", "y", "width", "height"}) {
        QJsonObject number;
        number["type"] = "integer";
        regionProperties[QLatin1String(key)] = number;
    }
    QJsonObject region;
    region["type"] = "object";
    region["description"] = "Part of the image to look at, in pixels of the document. Default: the whole image.";
    region["properties"] = regionProperties;
    region["required"] = QJsonArray {"x", "y", "width", "height"};

    QJsonObject question;
    question["type"] = "string";
    question["description"] = "What you want to know about the picture. Be specific.";

    QJsonObject selectionOnly;
    selectionOnly["type"] = "boolean";
    selectionOnly["description"] = "Look only at the current selection (instead of \"region\").";

    QJsonObject properties;
    properties["question"] = question;
    properties["region"] = region;
    properties["selection_only"] = selectionOnly;

    QJsonObject schema;
    schema["type"] = "object";
    schema["properties"] = properties;
    schema["required"] = QJsonArray {"question"};

    AiChatTool ask;
    ask.name = QStringLiteral("ask_about_image");
    ask.description = QStringLiteral(
        "Looks at the picture of the open document (all visible layers, as on the canvas) and answers a question "
        "about it, using a vision model. Use it to see: what is in the picture, how it looks, whether there is "
        "noise or blur, what colors dominate. Read-only. The picture is sent to the vision model "
        "at reduced size, so tiny details need a small \"region\".");
    ask.parameters = schema;
    ask.isAvailable = [](const AiChatRequestSettings &settings) { return settings.vision.isValid(); };
    ask.run = [executor, access, vision](const QJsonObject &arguments, const AiChatRequestSettings &settings, bool *ok) -> QString {
        *ok = false;

        const QString questionText = arguments.value("question").toString().trimmed();
        if (questionText.isEmpty()) {
            return i18n("\"question\" is required.");
        }

        QRect wantedRegion;
        QString error;
        if (!parseRegion(arguments.value("region"), &wantedRegion, &error)) {
            return error;
        }
        const bool selection = arguments.value("selection_only").toBool(false);

        QElapsedTimer timer;
        timer.start();

        QImage image;
        QRect used;
        bool rendered = false;
        qint64 renderInGuiMs = 0;
        if (!executor->run([&]() {
                QElapsedTimer renderTimer;
                renderTimer.start();
                rendered = access->renderImage(wantedRegion, selection, &image, &used, &error);
                renderInGuiMs = renderTimer.elapsed();
            }, &error)) {
            return error;
        }
        if (!rendered) {
            return error;
        }
        const qint64 renderMs = timer.elapsed(); // includes waiting until the GUI thread had time

        // the rest happens in the worker thread: scaling, encoding, waiting for the network
        const QImage flat = prepareForVision(image, kAiChatVisionMaxSide);
        const QByteArray png = AiChatVisionClient::encodePng(flat);
        const qint64 preparedMs = timer.elapsed();
        aiChatLog(QStringLiteral("ask_about_image: rendered %1x%2 px in %3 ms (%4 ms of it in the GUI thread), "
                                 "shrunk to %5x%6 and encoded to %7 KB PNG at %8 ms")
                      .arg(image.width()).arg(image.height()).arg(renderMs).arg(renderInGuiMs)
                      .arg(flat.width()).arg(flat.height()).arg(png.size() / 1024).arg(preparedMs));

        const QString answer = vision->ask(settings.vision, questionText, png, QStringLiteral("image/png"), &error);
        if (answer.isEmpty()) {
            return i18n("The vision model failed: %1", error);
        }

        *ok = true;
        return i18n("(Looked at the area x=%1 y=%2 width=%3 height=%4 of the image, shown at %5 x %6 pixels.)\n%7",
                    used.x(), used.y(), used.width(), used.height(), flat.width(), flat.height(), answer);
    };
    registry->add(ask);

    registry->setCancelHook([vision]() { vision->cancel(); });
}

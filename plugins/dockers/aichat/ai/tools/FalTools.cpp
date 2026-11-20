/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FalTools.h"

#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>

#include <klocalizedstring.h>

#include "DocumentTools.h"
#include "Fal.h"
#include "../../utils/Log.h"
#include "../../ui/Progress.h"
#include "Tools.h"
#include "Vision.h"

namespace AiChat {

namespace {

QJsonObject regionSchema()
{
    QJsonObject properties;
    for (const char *key : {"x", "y", "width", "height"}) {
        QJsonObject number;
        number["type"] = "integer";
        properties[QLatin1String(key)] = number;
    }
    QJsonObject region;
    region["type"] = "object";
    region["description"] = "Part of the image to process, in pixels of the document. Default: the whole image.";
    region["properties"] = properties;
    region["required"] = QJsonArray {"x", "y", "width", "height"};
    return region;
}

QJsonObject enumSchema(const QString &description, const QStringList &values)
{
    QJsonObject schema;
    schema["type"] = "string";
    schema["description"] = description;
    QJsonArray names;
    for (const QString &value : values) {
        names.append(value);
    }
    schema["enum"] = names;
    return schema;
}

QJsonObject unitSchema(const QString &description)
{
    QJsonObject schema;
    schema["type"] = "number";
    schema["minimum"] = 0.0;
    schema["maximum"] = 1.0;
    schema["description"] = description;
    return schema;
}

QJsonObject selectionSchema()
{
    QJsonObject schema;
    schema["type"] = "boolean";
    schema["description"] = "Process only the current selection (instead of \"region\").";
    return schema;
}

/// Copies the numbers 0..1 the model gave under `key` into `input`. False, with `*error`, if one is out of range.
bool copyUnitNumber(const QJsonObject &arguments, const char *key, QJsonObject *input, QString *error)
{
    const QJsonValue value = arguments.value(QLatin1String(key));
    if (value.isUndefined() || value.isNull()) {
        return true;
    }
    if (!value.isDouble() || value.toDouble() < 0.0 || value.toDouble() > 1.0) {
        *error = i18n("\"%1\" has to be a number from 0 to 1.", QString::fromLatin1(key));
        return false;
    }
    input->insert(QLatin1String(key), value.toDouble());
    return true;
}

bool copyChoice(const QJsonObject &arguments, const char *key, const QStringList &allowed, QJsonObject *input, QString *error)
{
    const QJsonValue value = arguments.value(QLatin1String(key));
    if (value.isUndefined() || value.isNull()) {
        return true;
    }
    if (!value.isString() || !allowed.contains(value.toString())) {
        *error = i18n("\"%1\" has to be one of: %2.", QString::fromLatin1(key), allowed.join(QStringLiteral(", ")));
        return false;
    }
    input->insert(QLatin1String(key), value.toString());
    return true;
}

struct FalTools {
    GuiExecutor *executor;
    DocumentAccess *access;
    FalClient *fal;
    Progress *progress;
};

/// The part both tools share: render, send, wait (with the progress window), put the result into a new layer.
/// `input` has the options already; `sizeFactor` is how much bigger the result gets; `layerName` names the new layer.
QString runTopaz(const FalTools &tools, const QString &endpoint, const QString &layerName, QJsonObject input, double sizeFactor,
                 const QJsonObject &arguments, const RequestSettings &settings, bool *ok)
{
    *ok = false;
    QString error;

    QRect wantedRegion;
    if (!parseRegion(arguments.value("region"), &wantedRegion, &error)) {
        return error;
    }
    const bool selection = arguments.value("selection_only").toBool(false);

    QElapsedTimer timer;
    timer.start();

    // ---- the picture, in the GUI thread
    QImage image;
    QRect used;
    QJsonObject documentInfo;
    bool rendered = false;
    if (!tools.executor->run([&]() {
            rendered = tools.access->documentInfo(&documentInfo, &error) && tools.access->renderImage(wantedRegion, selection, &image, &used, &error);
        }, &error)) {
        return error;
    }
    if (!rendered) {
        return error;
    }
    const QRect documentBounds(0, 0, documentInfo.value("width").toInt(), documentInfo.value("height").toInt());

    const double resultPixels = double(image.width()) * image.height() * sizeFactor * sizeFactor;
    if (resultPixels > kFalMaxResultPixels) {
        return i18n("The result would have %1 million pixels, which is too much (the limit is %2 million). "
                    "Use a smaller factor or a \"region\".",
                    qRound(resultPixels / 1e6), qRound(kFalMaxResultPixels / 1e6));
    }

    // ---- send, wait
    const QByteArray png = VisionClient::encodePng(image);
    input["output_format"] = QStringLiteral("png");
    writeLog(QStringLiteral("%1: rendered %2x%3 px, %4 KB PNG at %5 ms").arg(endpoint).arg(image.width()).arg(image.height())
                  .arg(png.size() / 1024).arg(timer.elapsed()));

    // The window belongs to the GUI thread; these calls only post events to it.
    Progress *progress = tools.progress;
    progress->beginLater(i18n("Sending the image to fal.ai"));
    const QByteArray bytes = tools.fal->run(settings.fal, endpoint, input,
                                            [progress](const QString &status) { progress->updateLater(status); }, &error,
                                            600000, 1000, png);
    progress->endLater();

    if (bytes.isEmpty()) {
        if (error == i18n("Canceled.")) {
            return i18n("The user canceled the job in the progress window. Do not start it again unless they ask.");
        }
        return i18n("fal.ai failed: %1", error);
    }
    const QImage result = QImage::fromData(bytes);
    if (result.isNull()) {
        return i18n("fal.ai sent a picture that could not be read.");
    }

    // ---- the result becomes a new layer, on top.
    //
    // The whole picture made bigger: the document grows to the size of the result (all its layers are
    // scaled with it) so that the new layer fits exactly. A part of the picture (a region, the selection)
    // is put back where it came from, at the size of the part: the document keeps its size.
    const bool wholeImage = used == documentBounds;
    const bool growDocument = wholeImage && sizeFactor != 1.0;
    QRect target = used;
    if (growDocument) {
        target = QRect(0, 0, qRound(documentBounds.width() * sizeFactor), qRound(documentBounds.height() * sizeFactor));
    }
    const QImage fitted = result.size() == target.size() ? result : result.scaled(target.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    QString summary;
    bool done = false;
    if (!tools.executor->run([&]() {
            QString step;
            if (growDocument) {
                LayerCommand scale;
                scale.type = LayerCommand::ScaleImage;
                scale.size = target.size();
                if (!tools.access->applyLayerCommand(scale, &step, &error)) {
                    return;
                }
                summary += step + QLatin1Char(' ');
            }
            LayerCommand add;
            add.type = LayerCommand::AddImageLayer;
            add.text = layerName;
            add.point = target.topLeft();
            add.image = fitted;
            done = tools.access->applyLayerCommand(add, &step, &error);
            summary += step;
        }, &error)) {
        return error;
    }
    if (!done) {
        return i18n("The result could not be put into the document: %1", error);
    }

    *ok = true;
    return i18n("Done. %1 It is on top of the other layers. The result is %2 x %3 pixels; the area that was processed is "
                "x=%4 y=%5 width=%6 height=%7. The original layers are unchanged%8. Each step is in the undo history.",
                summary, result.width(), result.height(), used.x(), used.y(), used.width(), used.height(),
                growDocument ? i18n(" (but they were scaled with the document)") : QString());
}

} // namespace

void addFalTools(ToolRegistry *registry, GuiExecutor *executor, DocumentAccess *access,
                       FalClient *fal, Progress *progress)
{
    const FalTools tools {executor, access, fal, progress};
    const auto available = [](const RequestSettings &settings) { return settings.fal.isValid(); };

    // ---- upscale_image
    const QStringList upscaleModels {"Standard V2", "High Fidelity V3", "High Fidelity V2", "Low Resolution V2", "CGI", "Text Refine", "Faces"};

    QJsonObject factor;
    factor["type"] = "number";
    factor["minimum"] = 1.0;
    factor["maximum"] = 16.0;
    factor["description"] = "How many times bigger the picture gets, in width and in height. Default 2.";

    QJsonObject face;
    face["type"] = "boolean";
    face["description"] = "Improve faces. Default true.";

    QJsonObject upscaleProperties;
    upscaleProperties["upscale_factor"] = factor;
    upscaleProperties["model"] = enumSchema(
        "Default \"Standard V2\" (general). \"High Fidelity V3\"/\"High Fidelity V2\": photos with fine detail. "
        "\"Low Resolution V2\": small or low quality sources. \"CGI\": renders and digital art. \"Text Refine\": text and "
        "graphics. \"Faces\": portraits.", upscaleModels);
    upscaleProperties["sharpen"] = unitSchema("Extra sharpening, 0 to 1. Leave out to let the model decide.");
    upscaleProperties["denoise"] = unitSchema("Noise removal, 0 to 1. Leave out to let the model decide.");
    upscaleProperties["fix_compression"] = unitSchema("Repair of JPEG artifacts, 0 to 1. Not for the CGI model.");
    upscaleProperties["face_enhancement"] = face;
    upscaleProperties["region"] = regionSchema();
    upscaleProperties["selection_only"] = selectionSchema();

    QJsonObject upscaleSchema;
    upscaleSchema["type"] = "object";
    upscaleSchema["properties"] = upscaleProperties;

    Tool upscale;
    upscale.name = QStringLiteral("upscale_image");
    upscale.description = QStringLiteral(
        "Makes the picture of the open document bigger and sharper with Topaz (on fal.ai). The picture, or the region "
        "or selection, is sent to fal.ai; the result becomes a NEW layer on top (the existing layers are not changed; if the whole picture is made bigger the document is scaled to fit). "
        "It takes from some seconds to over a minute, and a progress window blocks Krita meanwhile (the user can "
        "cancel it). It costs money: call it only when the user asks for it, once, not to try out settings.");
    upscale.parameters = upscaleSchema;
    upscale.isAvailable = available;
    upscale.run = [tools, upscaleModels](const QJsonObject &arguments, const RequestSettings &settings, bool *ok) -> QString {
        *ok = false;
        QString error;
        QJsonObject input;

        double scale = 2.0;
        const QJsonValue factorValue = arguments.value("upscale_factor");
        if (!factorValue.isUndefined() && !factorValue.isNull()) {
            scale = factorValue.toDouble(0.0);
            if (!factorValue.isDouble() || scale < 1.0 || scale > 16.0) {
                return i18n("\"upscale_factor\" has to be a number from 1 to 16.");
            }
        }
        input["upscale_factor"] = scale;

        if (!copyChoice(arguments, "model", upscaleModels, &input, &error)
            || !copyUnitNumber(arguments, "sharpen", &input, &error)
            || !copyUnitNumber(arguments, "denoise", &input, &error)
            || !copyUnitNumber(arguments, "fix_compression", &input, &error)) {
            return error;
        }
        if (arguments.value("face_enhancement").isBool()) {
            input["face_enhancement"] = arguments.value("face_enhancement").toBool();
        }

        return runTopaz(tools, QStringLiteral("topaz/upscale/image/precision"), i18n("Upscaled x%1", scale), input, scale, arguments,
                        settings, ok);
    };
    registry->add(upscale);

    // ---- denoise_image
    const QStringList denoiseModels {"Normal", "Strong", "Extreme", "Denoise Max"};

    QJsonObject denoiseProperties;
    denoiseProperties["model"] = enumSchema("How hard noise is removed. Default \"Normal\"; \"Strong\" and \"Extreme\" also remove "
                                            "detail; \"Denoise Max\" is the strongest.", denoiseModels);
    denoiseProperties["region"] = regionSchema();
    denoiseProperties["selection_only"] = selectionSchema();

    QJsonObject denoiseSchema;
    denoiseSchema["type"] = "object";
    denoiseSchema["properties"] = denoiseProperties;

    Tool denoise;
    denoise.name = QStringLiteral("denoise_image");
    denoise.description = QStringLiteral(
        "Removes noise from the picture of the open document with Topaz (on fal.ai). The picture, or the region or "
        "selection, is sent to fal.ai; the result becomes a NEW layer on top and the existing layers are not changed. "
        "A progress window blocks Krita meanwhile (the user can cancel it). It costs money: call it only when the user "
        "asks for it, once. You may look at the picture first with ask_about_image to see how noisy it is.");
    denoise.parameters = denoiseSchema;
    denoise.isAvailable = available;
    denoise.run = [tools, denoiseModels](const QJsonObject &arguments, const RequestSettings &settings, bool *ok) -> QString {
        *ok = false;
        QString error;
        QJsonObject input;
        if (!copyChoice(arguments, "model", denoiseModels, &input, &error)) {
            return error;
        }
        return runTopaz(tools, QStringLiteral("topaz/denoise/image"), i18n("Denoised"), input, 1.0, arguments, settings, ok);
    };
    registry->add(denoise);

    registry->addCancelHook([fal]() { fal->cancel(); });
}

} // namespace AiChat

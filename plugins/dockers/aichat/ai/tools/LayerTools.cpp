/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "LayerTools.h"

#include <QJsonArray>

#include <klocalizedstring.h>

#include "DocumentTools.h"
#include "Tools.h"

namespace AiChat {

namespace {

QJsonObject stringProperty(const char *description)
{
    QJsonObject property;
    property["type"] = "string";
    property["description"] = description;
    return property;
}

QJsonObject numberProperty(const char *type, const char *description)
{
    QJsonObject property;
    property["type"] = type;
    property["description"] = description;
    return property;
}

QJsonObject schemaOf(const QJsonObject &properties, const QStringList &required)
{
    QJsonObject schema;
    schema["type"] = "object";
    schema["properties"] = properties;
    QJsonArray names;
    for (const QString &name : required) {
        names.append(name);
    }
    schema["required"] = names;
    return schema;
}

const char *kLayerHelp = "The layer: its id (best) or its name, as get_document_info shows them.";

/// Makes the layer command from the arguments of the model; false, with `*error`, if they are not good.
using Builder = std::function<bool(const QJsonObject &arguments, LayerCommand *command, QString *error)>;

void addTool(ToolRegistry *registry, GuiExecutor *executor, DocumentAccess *access, const QString &name,
             const QString &description, const QJsonObject &schema, const Builder &build)
{
    Tool tool;
    tool.name = name;
    tool.description = description;
    tool.parameters = schema;
    tool.run = [executor, access, build](const QJsonObject &arguments, const RequestSettings &, bool *ok) -> QString {
        *ok = false;
        LayerCommand command;
        QString error;
        if (!build(arguments, &command, &error)) {
            return error;
        }

        QString summary;
        bool done = false;
        // the document belongs to the GUI thread
        if (!executor->run([&]() { done = access->applyLayerCommand(command, &summary, &error); }, &error)) {
            return error;
        }
        *ok = done;
        return done ? summary : error;
    };
    registry->add(tool);
}

bool needText(const QJsonObject &arguments, const char *key, QString *value, QString *error)
{
    const QJsonValue entry = arguments.value(QLatin1String(key));
    if (!entry.isString() || entry.toString().trimmed().isEmpty()) {
        *error = i18n("\"%1\" is required and has to be text.", QString::fromLatin1(key));
        return false;
    }
    *value = entry.toString();
    return true;
}

QString optionalText(const QJsonObject &arguments, const char *key)
{
    return arguments.value(QLatin1String(key)).toString();
}

bool needNumber(const QJsonObject &arguments, const char *key, double *value, QString *error)
{
    const QJsonValue entry = arguments.value(QLatin1String(key));
    if (!entry.isDouble()) {
        *error = i18n("\"%1\" is required and has to be a number.", QString::fromLatin1(key));
        return false;
    }
    *value = entry.toDouble();
    return true;
}

} // namespace

void addLayerTools(ToolRegistry *registry, GuiExecutor *executor, DocumentAccess *access)
{
    const QString undoNote = QStringLiteral(" It is one step of the undo history, so the user can take it back with Edit > Undo.");

    // ---- add_layer / add_group
    QJsonObject addProperties;
    addProperties["name"] = stringProperty("Name of the new layer. Default: the next free \"Layer N\".");
    addProperties["above"] = stringProperty("The layer the new one goes above (id or name). Default: on top of all layers.");
    addTool(registry, executor, access, QStringLiteral("add_layer"),
            QStringLiteral("Adds a new empty paint layer to the open document.") + undoNote, schemaOf(addProperties, {}),
            [](const QJsonObject &arguments, LayerCommand *command, QString *) {
                command->type = LayerCommand::AddLayer;
                command->text = optionalText(arguments, "name");
                command->layer = optionalText(arguments, "above");
                return true;
            });

    QJsonObject groupProperties;
    groupProperties["name"] = stringProperty("Name of the new group. Default: \"Group\".");
    groupProperties["above"] = stringProperty("The layer the new group goes above (id or name). Default: on top of all layers.");
    addTool(registry, executor, access, QStringLiteral("add_group"),
            QStringLiteral("Adds a new, empty layer group. Put layers into it with move_layer_to_group.") + undoNote,
            schemaOf(groupProperties, {}), [](const QJsonObject &arguments, LayerCommand *command, QString *) {
                command->type = LayerCommand::AddGroup;
                command->text = optionalText(arguments, "name");
                command->layer = optionalText(arguments, "above");
                return true;
            });

    // ---- rename_layer
    QJsonObject renameProperties;
    renameProperties["layer"] = stringProperty(kLayerHelp);
    renameProperties["name"] = stringProperty("The new name.");
    addTool(registry, executor, access, QStringLiteral("rename_layer"), QStringLiteral("Renames a layer.") + undoNote,
            schemaOf(renameProperties, {"layer", "name"}), [](const QJsonObject &arguments, LayerCommand *command, QString *error) {
                command->type = LayerCommand::Rename;
                return needText(arguments, "layer", &command->layer, error) && needText(arguments, "name", &command->text, error);
            });

    // ---- set_layer_visibility
    QJsonObject visibleProperties;
    visibleProperties["layer"] = stringProperty(kLayerHelp);
    visibleProperties["visible"] = numberProperty("boolean", "true shows the layer, false hides it.");
    addTool(registry, executor, access, QStringLiteral("set_layer_visibility"), QStringLiteral("Shows or hides a layer.") + undoNote,
            schemaOf(visibleProperties, {"layer", "visible"}), [](const QJsonObject &arguments, LayerCommand *command, QString *error) {
                command->type = LayerCommand::SetVisible;
                if (!arguments.value("visible").isBool()) {
                    *error = i18n("\"visible\" is required and has to be true or false.");
                    return false;
                }
                command->flag = arguments.value("visible").toBool();
                return needText(arguments, "layer", &command->layer, error);
            });

    // ---- set_layer_opacity
    QJsonObject opacityProperties;
    opacityProperties["layer"] = stringProperty(kLayerHelp);
    opacityProperties["opacity_percent"] = numberProperty("number", "0 (invisible) to 100 (opaque).");
    addTool(registry, executor, access, QStringLiteral("set_layer_opacity"), QStringLiteral("Sets the opacity of a layer.") + undoNote,
            schemaOf(opacityProperties, {"layer", "opacity_percent"}),
            [](const QJsonObject &arguments, LayerCommand *command, QString *error) {
                command->type = LayerCommand::SetOpacity;
                return needText(arguments, "layer", &command->layer, error) && needNumber(arguments, "opacity_percent", &command->number, error);
            });

    // ---- set_layer_blend_mode
    QJsonObject blendProperties;
    blendProperties["layer"] = stringProperty(kLayerHelp);
    blendProperties["mode"] = stringProperty("The blend mode id, e.g. normal, multiply, screen, overlay, darken, lighten, "
                                             "color_dodge, color_burn, hard_light, soft_light_svg, difference, exclusion, "
                                             "hue, saturation, color, luminosity, add, subtract, divide.");
    addTool(registry, executor, access, QStringLiteral("set_layer_blend_mode"), QStringLiteral("Sets how a layer blends with the ones below.") + undoNote,
            schemaOf(blendProperties, {"layer", "mode"}), [](const QJsonObject &arguments, LayerCommand *command, QString *error) {
                command->type = LayerCommand::SetBlendMode;
                return needText(arguments, "layer", &command->layer, error) && needText(arguments, "mode", &command->text, error);
            });

    // ---- move_layer
    QJsonObject moveProperties;
    moveProperties["layer"] = stringProperty(kLayerHelp);
    moveProperties["dx"] = numberProperty("integer", "Pixels to the right (negative: to the left).");
    moveProperties["dy"] = numberProperty("integer", "Pixels down (negative: up).");
    addTool(registry, executor, access, QStringLiteral("move_layer"), QStringLiteral("Moves the pixels of a layer on the canvas. Not for groups.") + undoNote,
            schemaOf(moveProperties, {"layer", "dx", "dy"}), [](const QJsonObject &arguments, LayerCommand *command, QString *error) {
                command->type = LayerCommand::MoveContent;
                double dx = 0;
                double dy = 0;
                if (!needText(arguments, "layer", &command->layer, error) || !needNumber(arguments, "dx", &dx, error)
                    || !needNumber(arguments, "dy", &dy, error)) {
                    return false;
                }
                command->point = QPoint(qRound(dx), qRound(dy));
                return true;
            });

    // ---- duplicate_layer
    QJsonObject layerOnly;
    layerOnly["layer"] = stringProperty(kLayerHelp);
    addTool(registry, executor, access, QStringLiteral("duplicate_layer"),
            QStringLiteral("Copies a layer; the copy is put above the original.") + undoNote, schemaOf(layerOnly, {"layer"}),
            [](const QJsonObject &arguments, LayerCommand *command, QString *error) {
                command->type = LayerCommand::Duplicate;
                return needText(arguments, "layer", &command->layer, error);
            });

    // ---- delete_layer
    addTool(registry, executor, access, QStringLiteral("delete_layer"),
            QStringLiteral("Deletes a layer (and what is inside it, for a group). Only when the user asked for it.") + undoNote,
            schemaOf(layerOnly, {"layer"}), [](const QJsonObject &arguments, LayerCommand *command, QString *error) {
                command->type = LayerCommand::Delete;
                return needText(arguments, "layer", &command->layer, error);
            });

    // ---- reorder_layer
    QJsonObject orderProperties;
    orderProperties["layer"] = stringProperty(kLayerHelp);
    QJsonObject to = stringProperty("Where the layer goes inside its group: top, bottom, up (one step) or down (one step).");
    to["enum"] = QJsonArray {"top", "bottom", "up", "down"};
    orderProperties["to"] = to;
    addTool(registry, executor, access, QStringLiteral("reorder_layer"), QStringLiteral("Changes the place of a layer in the stack.") + undoNote,
            schemaOf(orderProperties, {"layer", "to"}), [](const QJsonObject &arguments, LayerCommand *command, QString *error) {
                command->type = LayerCommand::Reorder;
                return needText(arguments, "layer", &command->layer, error) && needText(arguments, "to", &command->text, error);
            });

    // ---- move_layer_to_group
    QJsonObject groupMoveProperties;
    groupMoveProperties["layer"] = stringProperty(kLayerHelp);
    groupMoveProperties["group"] = stringProperty("The group (id or name) to move the layer into. Leave out to take it out of its group, "
                                                  "to the top level.");
    addTool(registry, executor, access, QStringLiteral("move_layer_to_group"), QStringLiteral("Puts a layer into a group.") + undoNote,
            schemaOf(groupMoveProperties, {"layer"}), [](const QJsonObject &arguments, LayerCommand *command, QString *error) {
                command->type = LayerCommand::MoveToGroup;
                command->text = optionalText(arguments, "group");
                return needText(arguments, "layer", &command->layer, error);
            });

    // ---- scale_image
    QJsonObject scaleProperties;
    scaleProperties["width"] = numberProperty("integer", "New width in pixels.");
    scaleProperties["height"] = numberProperty("integer", "New height in pixels. Keep the proportions unless the user wants otherwise.");
    addTool(registry, executor, access, QStringLiteral("scale_image"),
            QStringLiteral("Scales the whole document, with all its layers, to a new size in pixels. All layers lose some sharpness "
                           "when they are made bigger: for a better result use upscale_image.") + undoNote,
            schemaOf(scaleProperties, {"width", "height"}), [](const QJsonObject &arguments, LayerCommand *command, QString *error) {
                command->type = LayerCommand::ScaleImage;
                double width = 0;
                double height = 0;
                if (!needNumber(arguments, "width", &width, error) || !needNumber(arguments, "height", &height, error)) {
                    return false;
                }
                command->size = QSize(qRound(width), qRound(height));
                return true;
            });

    // ---- box_select
    QJsonObject boxProperties;
    for (const char *key : {"left", "top", "right", "bottom"}) {
        boxProperties[QLatin1String(key)] = numberProperty("number", "A fraction from 0 to 1 (not pixels). Numbers above 1 are refused.");
    }
    QJsonObject boxRegion;
    boxRegion["type"] = "object";
    boxRegion["description"] = "Only when the fractions are relative to a part of the image (the area ask_about_image said it looked at): "
                               "that area in pixels of the document. Default: the fractions are relative to the whole image.";
    QJsonObject regionNumbers;
    for (const char *key : {"x", "y", "width", "height"}) {
        regionNumbers[QLatin1String(key)] = QJsonObject {{"type", "integer"}};
    }
    boxRegion["properties"] = regionNumbers;
    boxRegion["required"] = QJsonArray {"x", "y", "width", "height"};
    boxProperties["region"] = boxRegion;

    Tool boxSelect;
    boxSelect.name = QStringLiteral("box_select");
    boxSelect.description = QStringLiteral(
        "Selects a rectangle of the open document with the rectangular selection (it replaces the current selection). "
        "The corners are fractions of the image from 0 to 1: left=0, top=0 is the top-left corner, right=1, bottom=1 the "
        "bottom-right corner. Fractions above 1 (or below 0) are refused. These are the same fractions ask_about_image "
        "gives for the position of things.") + undoNote;
    boxSelect.parameters = schemaOf(boxProperties, {"left", "top", "right", "bottom"});
    boxSelect.run = [executor, access](const QJsonObject &arguments, const RequestSettings &, bool *ok) -> QString {
        *ok = false;
        QString error;

        double edge[4] = {0, 0, 0, 0};
        const char *names[4] = {"left", "top", "right", "bottom"};
        for (int i = 0; i < 4; ++i) {
            if (!needNumber(arguments, names[i], &edge[i], &error)) {
                return error;
            }
            // fractions only: a pixel number would select the wrong place or nothing, so it is refused
            if (!(edge[i] >= 0.0 && edge[i] <= 1.0)) {
                return i18n("\"%1\" is %2, but it has to be a fraction from 0 to 1 (0 = left/top edge, 1 = right/bottom edge), "
                            "not pixels.", QString::fromLatin1(names[i]), edge[i]);
            }
        }
        if (edge[0] >= edge[2]) {
            return i18n("\"left\" (%1) has to be smaller than \"right\" (%2).", edge[0], edge[2]);
        }
        if (edge[1] >= edge[3]) {
            return i18n("\"top\" (%1) has to be smaller than \"bottom\" (%2).", edge[1], edge[3]);
        }

        QRect area;
        if (!parseRegion(arguments.value("region"), &area, &error)) {
            return error;
        }

        // the size of the document, to turn fractions into pixels (GUI thread)
        QJsonObject document;
        bool haveInfo = false;
        if (!executor->run([&]() { haveInfo = access->documentInfo(&document, &error); }, &error)) {
            return error;
        }
        if (!haveInfo) {
            return error;
        }
        const QRect bounds(0, 0, document.value("width").toInt(), document.value("height").toInt());
        if (area.isNull()) {
            area = bounds;
        } else if (!bounds.contains(area)) {
            return i18n("The region is not inside the image, which is %1 x %2 pixels.", bounds.width(), bounds.height());
        }

        const int left = area.x() + qRound(edge[0] * area.width());
        const int top = area.y() + qRound(edge[1] * area.height());
        const int right = area.x() + qRound(edge[2] * area.width());
        const int bottom = area.y() + qRound(edge[3] * area.height());
        if (right <= left || bottom <= top) {
            return i18n("The rectangle is smaller than one pixel.");
        }

        LayerCommand command;
        command.type = LayerCommand::SelectRect;
        command.rect = QRect(left, top, right - left, bottom - top);

        QString summary;
        bool done = false;
        if (!executor->run([&]() { done = access->applyLayerCommand(command, &summary, &error); }, &error)) {
            return error;
        }
        *ok = done;
        return done ? summary : error;
    };
    registry->add(boxSelect);
}

} // namespace AiChat

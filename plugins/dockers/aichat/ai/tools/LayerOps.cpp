/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "LayerOps.h"

#include <QUuid>

#include <KoColorProfile.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_layer_properties_icons.h>
#include <kis_default_bounds.h>
#include <KisImageResolutionProxy.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <kis_processing_applicator.h>
#include <kis_undo_adapter.h>
#include <kis_filter_strategy.h>
#include <klocalizedstring.h>

#include "commands/KisNodeRenameCommand.h"
#include "commands/kis_image_layer_add_command.h"
#include "commands/kis_image_layer_move_command.h"
#include "commands/kis_image_layer_remove_command.h"
#include "commands/kis_node_compositeop_command.h"
#include "commands/kis_node_opacity_command.h"
#include "commands/kis_set_global_selection_command.h"
#include "commands_new/kis_node_move_command2.h"

namespace AiChat {

namespace {

void collect(const KisNodeSP &parent, QList<KisNodeSP> *out)
{
    for (KisNodeSP child = parent->firstChild(); child; child = child->nextSibling()) {
        out->append(child);
        collect(child, out);
    }
}

QString idOf(const KisNodeSP &node)
{
    return node->uuid().toString(QUuid::WithoutBraces);
}

bool isGroup(const KisNodeSP &node)
{
    return qobject_cast<const KisGroupLayer *>(node.data()) != nullptr;
}

bool isInside(const KisNodeSP &node, const KisNodeSP &ancestor)
{
    for (KisNodeSP current = node; current; current = current->parent()) {
        if (current == ancestor) {
            return true;
        }
    }
    return false;
}

} // namespace

LayerOps::LayerOps(KisImageSP image)
    : m_image(std::move(image))
{
}

QString LayerOps::describe(const KisNodeSP &node)
{
    return QStringLiteral("\"%1\" (id %2)").arg(node->name(), idOf(node));
}

KisNodeSP LayerOps::resolve(const QString &reference, QString *error) const
{
    const QString wanted = reference.trimmed();
    if (wanted.isEmpty()) {
        *error = i18n("A layer is needed: its name or its id, as get_document_info shows them.");
        return KisNodeSP();
    }

    QList<KisNodeSP> nodes;
    collect(m_image->rootLayer(), &nodes);

    for (const KisNodeSP &node : nodes) {
        if (idOf(node).compare(wanted, Qt::CaseInsensitive) == 0) {
            return node;
        }
    }

    QList<KisNodeSP> matches;
    for (const KisNodeSP &node : nodes) {
        if (node->name() == wanted) {
            matches << node;
        }
    }
    if (matches.isEmpty()) {
        for (const KisNodeSP &node : nodes) {
            if (node->name().compare(wanted, Qt::CaseInsensitive) == 0) {
                matches << node;
            }
        }
    }

    if (matches.isEmpty()) {
        *error = i18n("There is no layer \"%1\". get_document_info lists the layers.", wanted);
        return KisNodeSP();
    }
    if (matches.size() > 1) {
        QStringList ids;
        for (const KisNodeSP &node : matches) {
            ids << idOf(node);
        }
        *error = i18n("More than one layer is named \"%1\". Use the id: %2", wanted, ids.join(QStringLiteral(", ")));
        return KisNodeSP();
    }
    return matches.first();
}

void LayerOps::runAsStroke(KUndo2Command *command)
{
    // the same way the layers docker changes a property: one entry of the history
    KisProcessingApplicator applicator(m_image, 0, KisProcessingApplicator::NONE, KisImageSignalVector(), command->text(), 0, command->id());
    applicator.applyCommand(command);
    applicator.end();
    m_image->waitForDone();
}

bool LayerOps::addNode(KisNodeSP node, const QString &aboveReference, QString *summary, QString *error)
{
    KisNodeSP parent = m_image->rootLayer();
    KisNodeSP above = parent->lastChild(); // on top of everything

    if (!aboveReference.trimmed().isEmpty()) {
        KisNodeSP reference = resolve(aboveReference, error);
        if (!reference) {
            return false;
        }
        parent = reference->parent();
        above = reference;
    }

    m_image->undoAdapter()->addCommand(new KisImageLayerAddCommand(m_image, node, parent, above));
    m_image->waitForDone();
    if (!node->parent()) {
        *error = i18n("The layer could not be added.");
        return false;
    }
    *summary = i18n("Added the layer %1.", describe(node));
    return true;
}

bool LayerOps::apply(const LayerCommand &command, QString *summary, QString *error)
{
    if (!m_image) {
        *error = i18n("There is no open document.");
        return false;
    }

    const KoColorSpace *colorSpace = m_image->colorSpace();

    switch (command.type) {
    case LayerCommand::AddLayer: {
        const QString name = command.text.trimmed().isEmpty() ? m_image->nextLayerName() : command.text.trimmed();
        KisNodeSP layer = new KisPaintLayer(m_image.data(), name, OPACITY_OPAQUE_U8, colorSpace);
        return addNode(layer, command.layer, summary, error);
    }
    case LayerCommand::AddGroup: {
        const QString name = command.text.trimmed().isEmpty() ? i18n("Group") : command.text.trimmed();
        KisNodeSP group = new KisGroupLayer(m_image.data(), name, OPACITY_OPAQUE_U8);
        return addNode(group, command.layer, summary, error);
    }
    case LayerCommand::AddImageLayer: {
        if (command.image.isNull()) {
            *error = i18n("There is no picture for the layer.");
            return false;
        }
        const QString name = command.text.trimmed().isEmpty() ? m_image->nextLayerName() : command.text.trimmed();
        KisPaintLayerSP layer = new KisPaintLayer(m_image.data(), name, OPACITY_OPAQUE_U8, colorSpace);
        const KoColorProfile *profile = KoColorSpaceRegistry::instance()->rgb8()->profile();
        layer->paintDevice()->convertFromQImage(command.image.convertToFormat(QImage::Format_ARGB32), profile, command.point.x(), command.point.y());
        return addNode(layer, QString(), summary, error);
    }
    case LayerCommand::ScaleImage: {
        const double pixels = double(command.size.width()) * command.size.height();
        if (command.size.width() < 1 || command.size.height() < 1 || pixels > kMaxDocumentPixels) {
            *error = i18n("The size %1 x %2 is not possible (the document may have up to %3 million pixels).",
                          command.size.width(), command.size.height(), qRound(kMaxDocumentPixels / 1e6));
            return false;
        }
        if (command.size == QSize(m_image->width(), m_image->height())) {
            *error = i18n("The document already has this size.");
            return false;
        }
        KisFilterStrategy *strategy = KisFilterStrategyRegistry::instance()->value(QStringLiteral("Bicubic"));
        m_image->scaleImage(command.size, m_image->xRes(), m_image->yRes(), strategy);
        m_image->waitForDone();
        *summary = i18n("The document, with all its layers, now is %1 x %2 pixels.", m_image->width(), m_image->height());
        return true;
    }
    case LayerCommand::SelectRect: {
        const QRect rect = command.rect & m_image->bounds();
        if (rect.isEmpty()) {
            *error = i18n("The rectangle is outside of the image, which is %1 x %2 pixels.", m_image->width(), m_image->height());
            return false;
        }
        // like "Select All": a selection that starts empty, filled with the rectangle
        KisSelectionSP selection = new KisSelection(new KisSelectionEmptyBounds(m_image), toQShared(new KisImageResolutionProxy(m_image)));
        selection->pixelSelection()->select(rect);
        KUndo2Command *set = new KisSetGlobalSelectionCommand(m_image, selection);
        set->setText(kundo2_i18n("Select Rectangle"));
        runAsStroke(set);
        *summary = i18n("Selected the rectangle x=%1 y=%2 width=%3 height=%4 (pixels of the document).", rect.x(), rect.y(), rect.width(), rect.height());
        return true;
    }
    default:
        break;
    }

    // everything else works on one layer
    KisNodeSP node = resolve(command.layer, error);
    if (!node) {
        return false;
    }

    switch (command.type) {
    case LayerCommand::Rename: {
        const QString name = command.text.trimmed();
        if (name.isEmpty()) {
            *error = i18n("The new name must not be empty.");
            return false;
        }
        const QString old = node->name();
        runAsStroke(new KisNodeRenameCommand(node, old, name));
        *summary = i18n("Renamed the layer \"%1\" to \"%2\".", old, name);
        return true;
    }
    case LayerCommand::SetVisible: {
        KisLayerPropertiesIcons::setNodePropertyAutoUndo(node, KisLayerPropertiesIcons::visible, command.flag, m_image);
        m_image->waitForDone();
        *summary = command.flag ? i18n("The layer %1 is shown.", describe(node)) : i18n("The layer %1 is hidden.", describe(node));
        return true;
    }
    case LayerCommand::SetOpacity: {
        if (command.number < 0.0 || command.number > 100.0) {
            *error = i18n("The opacity has to be between 0 and 100 percent.");
            return false;
        }
        runAsStroke(new KisNodeOpacityCommand(node, quint8(qRound(command.number * 255.0 / 100.0))));
        *summary = i18n("The layer %1 now has %2 percent opacity.", describe(node), qRound(command.number));
        return true;
    }
    case LayerCommand::SetBlendMode: {
        const QString mode = command.text.trimmed();
        if (mode.isEmpty() || !colorSpace->hasCompositeOp(mode)) {
            *error = i18n("\"%1\" is not a blend mode of this document. Examples: normal, multiply, screen, overlay, "
                          "darken, lighten, color_dodge, color_burn, hard_light, soft_light_svg, difference, exclusion, "
                          "hue, saturation, color, luminosity, add, subtract, divide.", mode);
            return false;
        }
        runAsStroke(new KisNodeCompositeOpCommand(node, mode));
        *summary = i18n("The layer %1 now blends with \"%2\".", describe(node), mode);
        return true;
    }
    case LayerCommand::MoveContent: {
        if (isGroup(node)) {
            *error = i18n("A group has no pixels of its own. Move the layers inside it.");
            return false;
        }
        if (node->userLocked()) {
            *error = i18n("The layer %1 is locked.", describe(node));
            return false;
        }
        if (command.point.isNull()) {
            *error = i18n("The offset is zero: nothing to move.");
            return false;
        }
        const QPoint from(node->x(), node->y());
        runAsStroke(new KisNodeMoveCommand2(node, from, from + command.point));
        *summary = i18n("Moved the layer %1 by %2, %3 pixels.", describe(node), command.point.x(), command.point.y());
        return true;
    }
    case LayerCommand::Duplicate: {
        KisNodeSP copy = node->clone();
        if (!copy) {
            *error = i18n("This layer cannot be duplicated.");
            return false;
        }
        copy->setName(i18n("%1 (copy)", node->name()));
        m_image->undoAdapter()->addCommand(new KisImageLayerAddCommand(m_image, copy, node->parent(), node));
        m_image->waitForDone();
        if (!copy->parent()) {
            *error = i18n("The layer could not be duplicated.");
            return false;
        }
        *summary = i18n("Duplicated the layer %1; the copy is %2.", describe(node), describe(copy));
        return true;
    }
    case LayerCommand::Delete: {
        if (node->userLocked()) {
            *error = i18n("The layer %1 is locked.", describe(node));
            return false;
        }
        const QString what = describe(node);
        m_image->undoAdapter()->addCommand(new KisImageLayerRemoveCommand(m_image, node));
        m_image->waitForDone();
        *summary = i18n("Deleted the layer %1 (Edit > Undo brings it back).", what);
        return true;
    }
    case LayerCommand::Reorder: {
        KisNodeSP parent = node->parent();
        KisNodeSP above;
        const QString to = command.text.trimmed().toLower();
        if (to == QLatin1String("top")) {
            above = parent->lastChild();
            if (above == node) {
                *error = i18n("The layer is already on top.");
                return false;
            }
        } else if (to == QLatin1String("bottom")) {
            if (!node->prevSibling()) {
                *error = i18n("The layer is already at the bottom.");
                return false;
            }
            // above = nothing: the very bottom
        } else if (to == QLatin1String("up")) {
            above = node->nextSibling();
            if (!above) {
                *error = i18n("The layer is already on top.");
                return false;
            }
        } else if (to == QLatin1String("down")) {
            KisNodeSP below = node->prevSibling();
            if (!below) {
                *error = i18n("The layer is already at the bottom.");
                return false;
            }
            above = below->prevSibling();
        } else {
            *error = i18n("\"%1\" is not a place. Use top, bottom, up or down.", command.text);
            return false;
        }
        m_image->undoAdapter()->addCommand(new KisImageLayerMoveCommand(m_image, node, parent, above));
        m_image->waitForDone();
        *summary = i18n("Moved the layer %1 to the place \"%2\".", describe(node), to);
        return true;
    }
    case LayerCommand::MoveToGroup: {
        KisNodeSP target = m_image->rootLayer();
        if (!command.text.trimmed().isEmpty()) {
            target = resolve(command.text, error);
            if (!target) {
                return false;
            }
            if (!isGroup(target)) {
                *error = i18n("The layer %1 is not a group.", describe(target));
                return false;
            }
            if (isInside(target, node)) {
                *error = i18n("A group cannot be moved into itself.");
                return false;
            }
        }
        if (node->parent() == target) {
            *error = i18n("The layer is already there.");
            return false;
        }
        m_image->undoAdapter()->addCommand(new KisImageLayerMoveCommand(m_image, node, target, target->lastChild()));
        m_image->waitForDone();
        *summary = target.data() == m_image->rootLayer().data() ? i18n("Moved the layer %1 out of its group, to the top level.", describe(node))
                                                  : i18n("Moved the layer %1 into the group %2.", describe(node), describe(target));
        return true;
    }
    default:
        break;
    }

    *error = i18n("This operation is not known.");
    return false;
}

} // namespace AiChat

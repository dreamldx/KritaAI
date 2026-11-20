/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_TOOLS_LAYER_OPS_H_
#define _AI_TOOLS_LAYER_OPS_H_

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include <kis_types.h>

namespace AiChat {

/// The most pixels a document may have after "scale_image" (a mistake in a number must not eat all memory).
constexpr double kMaxDocumentPixels = 200.0 * 1000 * 1000;

/**
 * One change of the layers of a document that the AI asked for.
 * Plain data: the tools fill it in, Krita (or a fake in the tests) carries it out in the GUI thread.
 */
struct LayerCommand {
    enum Type {
        AddLayer, ///< a new empty paint layer: `text` is the name, `layer` (optional) the one it goes above
        AddGroup, ///< a new group: `text` is the name, `layer` (optional) the one it goes above
        AddImageLayer, ///< a paint layer with `image` at `point`: `text` is the name; goes on top
        Rename, ///< `layer` gets the name `text`
        SetVisible, ///< `layer` is shown or hidden (`flag`)
        SetOpacity, ///< `layer` gets `number` percent opacity
        SetBlendMode, ///< `layer` gets the blend mode `text` (an id such as "multiply")
        MoveContent, ///< the pixels of `layer` move by `point`
        Duplicate, ///< `layer` is copied, the copy goes above it
        Delete, ///< `layer` is removed
        Reorder, ///< `layer` goes to `text`: "top", "bottom", "up" or "down" (within its group)
        MoveToGroup, ///< `layer` goes into the group `text` (empty: to the top level)
        ScaleImage, ///< the whole document, with all its layers, is scaled to `size`
        SelectRect, ///< the selection of the document becomes the rectangle `rect` (pixels of the document)
    };

    Type type {AddLayer};
    QString layer; ///< id or name, as get_document_info shows them
    QString text;
    qreal number {0};
    bool flag {true};
    QPoint point;
    QSize size;
    QRect rect;
    QImage image;
};

/**
 * Carries out an LayerCommand on an image of Krita. Every command is one entry of the undo
 * history of the document (Edit > Undo takes it back), so the user can always step back from what the
 * AI did. Use it in the GUI thread.
 */
class LayerOps
{
public:
    explicit LayerOps(KisImageSP image);

    /// True if it was done; `*summary` says what happened. Otherwise `*error` says why not (nothing was changed).
    bool apply(const LayerCommand &command, QString *summary, QString *error);

    /// The layer an id or a name stands for. Null, with `*error`, if there is none or the name is not unique.
    KisNodeSP resolve(const QString &reference, QString *error) const;

    /// "\"Name\" (id ...)": how a layer is named in answers.
    static QString describe(const KisNodeSP &node);

private:
    bool addNode(KisNodeSP node, const QString &aboveReference, QString *summary, QString *error);
    void runAsStroke(KUndo2Command *command);

    KisImageSP m_image;
};

} // namespace AiChat

#endif

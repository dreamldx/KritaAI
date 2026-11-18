/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _AI_CHAT_DOCUMENT_TOOLS_H_
#define _AI_CHAT_DOCUMENT_TOOLS_H_

#include <QImage>
#include <QJsonObject>
#include <QRect>
#include <QString>

class AiChatGuiExecutor;
class AiChatToolRegistry;
class AiChatVisionClient;

/**
 * What the read-only tools need to know about Krita. Every function is called in
 * the GUI thread (through an AiChatGuiExecutor). The tests use a fake.
 */
class AiChatDocumentAccess
{
public:
    virtual ~AiChatDocumentAccess() = default;

    /// A description of the active document. False, with `*error`, if there is none.
    virtual bool documentInfo(QJsonObject *info, QString *error) = 0;

    /// Renders the picture of the active document (what you see on the canvas, all layers).
    /// `region` (in pixels of the document) restricts it; a null region means everything.
    /// `selectionOnly` restricts it to the bounds of the current selection.
    /// `*usedRegion` tells what was rendered.
    virtual bool renderImage(const QRect &region, bool selectionOnly, QImage *image, QRect *usedRegion, QString *error) = 0;
};

/// The real thing: the document that is active in the active window of Krita.
class KritaDocumentAccess : public AiChatDocumentAccess
{
public:
    bool documentInfo(QJsonObject *info, QString *error) override;
    bool renderImage(const QRect &region, bool selectionOnly, QImage *image, QRect *usedRegion, QString *error) override;
};

/// Longest side of the picture that is sent to the vision model.
constexpr int kAiChatVisionMaxSide = 1024;

/**
 * Adds the read-only tools to the registry:
 *
 *  - get_document_info: size, colors, layers, selection of the active document.
 *  - ask_about_image(question, region?, selection_only?): a vision model answers a
 *    question about the picture. Only offered while a vision model is set up.
 *
 * None of the pointers is owned; all must outlive the registry.
 */
void aiChatAddDocumentTools(AiChatToolRegistry *registry, AiChatGuiExecutor *executor, AiChatDocumentAccess *access,
                            AiChatVisionClient *vision);

#endif

/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AiChat.h"

#include <kpluginfactory.h>
#include <klocalizedstring.h>

#include <KoDockFactoryBase.h>
#include <KoDockRegistry.h>
#include <kis_preference_set_registry.h>

#include "AiChatDock.h"
#include "AiChatPreferences.h"

K_PLUGIN_FACTORY_WITH_JSON(AiChatPluginFactory, "kritaaichatdocker.json", registerPlugin<AiChatPlugin>();)

class AiChatDockFactory : public KoDockFactoryBase
{
public:
    QString id() const override
    {
        return QString("AiChat");
    }

    QDockWidget *createDockWidget() override
    {
        AiChatDock *dockWidget = new AiChatDock();
        dockWidget->setObjectName(id());
        return dockWidget;
    }

    DockPosition defaultDockPosition() const override
    {
        return DockRight;
    }
};

AiChatPlugin::AiChatPlugin(QObject *parent, const QVariantList &)
    : QObject(parent)
{
    KoDockRegistry::instance()->add(new AiChatDockFactory());

    // the "AI Chat" page of Configure Krita
    KisPreferenceSetRegistry::instance()->add("AiChatPreferencesFactory", new AiChatPreferencesFactory());
}

AiChatPlugin::~AiChatPlugin()
{
}

#include <AiChat.moc>

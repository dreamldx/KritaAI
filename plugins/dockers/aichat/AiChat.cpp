/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AiChat.h"

#include <kpluginfactory.h>
#include <klocalizedstring.h>

#include <KoDockFactoryBase.h>
#include <KoDockRegistry.h>
#include <kis_preference_set_registry.h>

#include "ui/Dock.h"
#include "ui/Preferences.h"

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
        return new AiChat::Dock(); // Krita sets the object name from id()
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
    KisPreferenceSetRegistry::instance()->add("AiChatPreferencesFactory", new AiChat::PreferencesFactory());
}

AiChatPlugin::~AiChatPlugin()
{
}

#include <AiChat.moc>

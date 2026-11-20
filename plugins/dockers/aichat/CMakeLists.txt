add_subdirectory(tests)

# All the logic lives in a static library, so the tests can link it.
set(kritaaichatdocker_static_SOURCES
    ai/Agent.h
    ai/Agent.cpp
    network/Client.h
    network/Client.cpp
    config/Config.h
    config/Config.cpp
    model/Chat.h
    model/Chat.cpp
    model/Request.h
    model/Request.cpp
    ui/Dock.h
    ui/Dock.cpp
    ui/MessageWidget.h
    ai/tools/DocumentTools.h
    ai/tools/DocumentTools.cpp
    ai/tools/Fal.h
    ai/tools/Fal.cpp
    ai/tools/FalTools.h
    ai/tools/FalTools.cpp
    ai/tools/LayerOps.h
    ai/tools/LayerOps.cpp
    ai/tools/LayerTools.h
    ai/tools/LayerTools.cpp
    utils/Log.h
    utils/Log.cpp
    ui/Progress.h
    ui/Progress.cpp
    ui/Preferences.h
    ui/Preferences.cpp
    utils/Store.h
    utils/Store.cpp
    network/Stream.h
    network/Stream.cpp
    ai/tools/Tools.h
    ai/tools/Tools.cpp
    ai/tools/Vision.h
    ai/tools/Vision.cpp
    utils/Worker.h
    utils/Worker.cpp
)

kis_add_library(kritaaichatdocker_static STATIC ${kritaaichatdocker_static_SOURCES})
target_link_libraries(kritaaichatdocker_static kritaui Qt5::Network Qt5::Sql)

# The plugin itself only registers the docker (and the settings page).
kis_add_library(kritaaichatdocker MODULE AiChat.h AiChat.cpp)
target_link_libraries(kritaaichatdocker kritaaichatdocker_static)
install(TARGETS kritaaichatdocker DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})

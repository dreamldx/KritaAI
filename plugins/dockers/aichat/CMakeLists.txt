add_subdirectory(tests)

# All the logic lives in a static library, so the tests can link it.
set(kritaaichatdocker_static_SOURCES
    AiChatAgent.h
    AiChatAgent.cpp
    AiChatClient.h
    AiChatClient.cpp
    AiChatConfig.h
    AiChatConfig.cpp
    AiChatDock.h
    AiChatDock.cpp
    AiChatDocumentTools.h
    AiChatDocumentTools.cpp
    AiChatLog.h
    AiChatLog.cpp
    AiChatPreferences.h
    AiChatPreferences.cpp
    AiChatStore.h
    AiChatStore.cpp
    AiChatStream.h
    AiChatStream.cpp
    AiChatTools.h
    AiChatTools.cpp
    AiChatTypes.h
    AiChatVision.h
    AiChatVision.cpp
    AiChatWorker.h
    AiChatWorker.cpp
)

kis_add_library(kritaaichatdocker_static STATIC ${kritaaichatdocker_static_SOURCES})
target_link_libraries(kritaaichatdocker_static kritaui Qt5::Network Qt5::Sql)

# The plugin itself only registers the docker (and the settings page).
kis_add_library(kritaaichatdocker MODULE AiChat.h AiChat.cpp)
target_link_libraries(kritaaichatdocker kritaaichatdocker_static)
install(TARGETS kritaaichatdocker DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})

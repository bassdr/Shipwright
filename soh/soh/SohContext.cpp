#include "soh/SohContext.h"
#include "soh/OTRGlobals.h"

#include <ship/audio/Audio.h>
#include <ship/config/Config.h>
#include <ship/config/ConsoleVariable.h>
#include <ship/controller/controldeck/ControlDeck.h>
#include <ship/core/Context.h>
#include <ship/debug/Console.h>
#include <ship/resource/ResourceManager.h>
#include <ship/window/Window.h>

Ship::Context* SohContext() {
    return OTRGlobals::Instance->context;
}

namespace {
// Re-resolved if the context is ever replaced, which SoH does not do today.
template <typename T> std::shared_ptr<T> Cached() {
    static Ship::Context* owner = nullptr;
    static std::shared_ptr<T> cached;
    Ship::Context* current = SohContext();
    if (owner != current || cached == nullptr) {
        owner = current;
        cached = current != nullptr ? current->GetFirstInChildren<T>() : nullptr;
    }
    return cached;
}
} // namespace

std::shared_ptr<Ship::Window> SohWindow() {
    return Cached<Ship::Window>();
}
std::shared_ptr<Ship::ResourceManager> SohResourceManager() {
    return Cached<Ship::ResourceManager>();
}
std::shared_ptr<Ship::ControlDeck> SohControlDeck() {
    return Cached<Ship::ControlDeck>();
}
std::shared_ptr<Ship::Config> SohConfig() {
    return Cached<Ship::Config>();
}
std::shared_ptr<Ship::Audio> SohAudio() {
    return Cached<Ship::Audio>();
}
std::shared_ptr<Ship::Console> SohConsole() {
    return Cached<Ship::Console>();
}

std::shared_ptr<Ship::ConsoleVariable> SohConsoleVariables() {
    return Cached<Ship::ConsoleVariable>();
}

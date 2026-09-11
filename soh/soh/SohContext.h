#pragma once

#include <memory>

#include <ship/audio/Audio.h>
#include <ship/config/Config.h>
#include <ship/config/ConsoleVariable.h>
#include <ship/controller/controldeck/ControlDeck.h>
#include <ship/core/Context.h>
#include <ship/debug/Console.h>
#include <ship/resource/ResourceManager.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>

/**
 * @brief The context and subsystems libultraship no longer exposes statically.
 *
 * The component system removed Context's static instance and its subsystem getters;
 * components are meant to find each other by walking the hierarchy. SoH holds a
 * pointer from startup, so these return it and its children directly.
 *
 * The lookups are cached: GetFirstInChildren() runs a breadth-first search with a
 * fresh visited set and a dynamic_pointer_cast per node, and several of these are
 * called every frame.
 *
 * This pulls in the subsystem headers rather than forward declaring them, because it
 * stands in for ship/Context.h, which callers relied on to do the same.
 */
Ship::Context* SohContext();

std::shared_ptr<Ship::Window> SohWindow();
std::shared_ptr<Ship::ResourceManager> SohResourceManager();
std::shared_ptr<Ship::ControlDeck> SohControlDeck();
std::shared_ptr<Ship::Config> SohConfig();
std::shared_ptr<Ship::Audio> SohAudio();
std::shared_ptr<Ship::Console> SohConsole();
std::shared_ptr<Ship::ConsoleVariable> SohConsoleVariables();

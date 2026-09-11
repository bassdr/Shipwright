#pragma once

#include <fast/Fast3dWindow.h>
#include <ship/audio/Audio.h>

static const std::map<Ship::AudioBackend, const char*> audioBackendsMap = {
    { Ship::AudioBackend::SDL, "SDL" },
    { Ship::AudioBackend::NUL, "Null" },
};

static const std::map<Fast::WindowBackend, const char*> windowBackendsMap = {
    { Fast::WindowBackend::FAST3D_DXGI_DX11, "DirectX" },
    { Fast::WindowBackend::FAST3D_SDL_OPENGL, "OpenGL" },
    { Fast::WindowBackend::FAST3D_SDL_METAL, "Metal" },
};

#pragma once

#include "soh/SohGui/SohGuiWindow.h"
#include <ship/window/gui/GuiWindow.h>

class HookDebuggerWindow final : public SohGui::Window {
  public:
    using SohGui::Window::Window;

    void InitElement() override;
    void DrawElement() override;
    void UpdateElement() override{};
};

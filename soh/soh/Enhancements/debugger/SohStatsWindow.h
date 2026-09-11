#pragma once

#include "soh/SohGui/SohGuiWindow.h"
#include <ship/window/gui/GuiWindow.h>

class SohStatsWindow final : public SohGui::Window {
  public:
    using SohGui::Window::Window;
    ~SohStatsWindow(){};

  protected:
    void InitElement() override{};
    void DrawElement() override;
    void UpdateElement() override{};
};

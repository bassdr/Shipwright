#pragma once

#include "soh/SohGui/SohGuiWindow.h"
#include <ship/window/gui/GuiWindow.h>

#include "z64actor.h"

#include <vector>

class ActorViewerWindow final : public SohGui::Window {
  public:
    using SohGui::Window::Window;

    void DrawElement() override;
    void InitElement() override;
    void UpdateElement() override{};

  private:
    Actor* display = nullptr;
    int category = ACTORCAT_SWITCH;
    std::vector<Actor*> list;
};

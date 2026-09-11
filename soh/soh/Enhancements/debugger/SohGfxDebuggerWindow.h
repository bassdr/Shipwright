#pragma once

#include <nlohmann/json.hpp>
#include <libultraship/window/gui/GfxDebuggerWindow.h>

class SohGfxDebuggerWindow : public LUS::GfxDebuggerWindow {
  public:
    using GfxDebuggerWindow::GfxDebuggerWindow;

  protected:
    void OnInit(const nlohmann::json& initArgs = nlohmann::json::object()) override;
    void UpdateElement() override;
    void DrawElement() override;
};

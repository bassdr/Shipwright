#pragma once

#include "soh/SohGui/SohGuiWindow.h"
#include <ship/window/gui/GuiWindow.h>

typedef enum { COLVIEW_DISABLED, COLVIEW_SOLID, COLVIEW_TRANSPARENT } ColViewerRenderSetting;

#ifdef __cplusplus
class ColViewerWindow final : public SohGui::Window {
  public:
    using SohGui::Window::Window;

    void InitElement() override;
    void DrawElement() override;
    void UpdateElement() override{};
};

#endif

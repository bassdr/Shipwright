#include "SohGfxDebuggerWindow.h"
#include "soh/OTRGlobals.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include "soh/cvar_prefixes.h"

void SohGfxDebuggerWindow::OnInit(const nlohmann::json& initArgs) {
    LUS::GfxDebuggerWindow::OnInit(initArgs);
    GfxDebuggerWindow::InitElement();
}

void SohGfxDebuggerWindow::UpdateElement() {
    GfxDebuggerWindow::UpdateElement();
}

void SohGfxDebuggerWindow::DrawElement() {
    ImGui::BeginDisabled(CVarGetInteger(CVAR_SETTING("DisableChanges"), 0));
    ImGui::PushFont(OTRGlobals::Instance->fontMonoLarger);
    GfxDebuggerWindow::DrawElement();
    ImGui::PopFont();
    ImGui::EndDisabled();
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Rajesh D'Monte
// No-op implementation of the console overlay for non-Windows targets.
// This stub lets the engine keep including ConsoleOverlay.h and calling
// its methods unconditionally where no native overlay implementation
// exists. Elsewhere we lean on the web console
// (http://127.0.0.1:27960) for live cvar editing and use the engine's
// terminal logging instead.

#include "ConsoleOverlay.h"

namespace pt::app {

ConsoleOverlay::ConsoleOverlay()  {}
ConsoleOverlay::~ConsoleOverlay() {}

bool ConsoleOverlay::Init(void*) { return false; }
void ConsoleOverlay::Shutdown()  {}

void ConsoleOverlay::Show()                                    {}
void ConsoleOverlay::Hide()                                    {}
void ConsoleOverlay::Toggle()                                  {}
bool ConsoleOverlay::IsShown() const                           { return false; }
void ConsoleOverlay::ApplyTheme(std::string_view)              {}
// Repaint() is wired by Engine.cpp's r_theme / con_font_scale cvar
// on_change handlers, called unconditionally from Console::Drain
// regardless of host overlay support.  No native overlay on Linux,
// so just swallow.
void ConsoleOverlay::Repaint()                                 {}
void ConsoleOverlay::NotifyParentResized(int, int)             {}
void ConsoleOverlay::OnLog(pt::log::Level, const std::string&) {}
void ConsoleOverlay::SetGlobalInstance(ConsoleOverlay*)        {}
bool ConsoleOverlay::SaveState(const std::string&) const       { return false; }
bool ConsoleOverlay::LoadState(const std::string&)             { return false; }

}  // namespace pt::app

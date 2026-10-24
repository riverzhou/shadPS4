// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Libraries::Pad {

// Which screen the title is showing, as far as the automated (controller-less) input path needs
// to know. The title itself does not expose this through an API; it is derived from the title's
// own UI log. Callers must only inject directional input in the StartupSettings state, because a
// direction pressed on a list changes the selection.
enum class MenuScreen {
    Unknown,
    StartupSettings,
    IntroMovie,
};

// Returns the last known screen. Unknown means "no reliable information": callers must fall back
// to confirm-only input. The state is updated by a background watcher that starts on first call
// and is a no-op when the title does not write a UI log.
MenuScreen GetMenuScreen();

/// Number of display-settings pages that have logged a finalize line. The wizard is over at three;
/// automated input uses this to stop pressing instead of walking the menus behind it.
int SettingsPagesFinalized();

} // namespace Libraries::Pad

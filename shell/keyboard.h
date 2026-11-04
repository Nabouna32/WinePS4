// WinePS4's text entry for the desktop: the PS4's own keyboard dialog (the controller types on it,
// and a USB keyboard types into it). From HENkaku's ps4_keyboard.cpp.
#pragma once

#include <string>

namespace shell {

// Opens the keyboard with a title and the text to start from (password: it shows dots); false if the
// system refused
bool keyboard_open(const std::string &title, const std::string &text, bool password = false);
// Whether it is open (the desktop's input waits meanwhile)
bool keyboard_active();
// Once per frame: 1 with the text when the user finished, -1 when they cancelled, 0 otherwise
int keyboard_poll(std::string &text);

} // namespace shell

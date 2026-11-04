// WinePS4's program icons for the desktop's Files: the icon inside a Windows program (or an .ico
// file), read from the file itself.
#pragma once

#include <string>

#include "ps4_canvas.h"

namespace shell {

// The icon a Windows program file carries (.exe, .dll, .cpl, .scr, .ico): the image nearest to size,
// scaled to size x size, straight-alpha RGBA as ps4ui::Image. Empty when there is none or the file
// can't be read.
ps4ui::Image file_icon(const std::string &path, int size);

} // namespace shell

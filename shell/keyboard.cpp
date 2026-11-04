// WinePS4's text entry (keyboard.h): sceImeDialog, as HENkaku's ps4_keyboard.cpp opens it. Its
// text is UTF-16 (the PS4's wchar_t is 16 bits), the desktop's UTF-8.
#include "keyboard.h"
#include "hwkeyboard.h"

#include <cstdint>
#include <cstdio>

#include <orbis/CommonDialog.h>
#include <orbis/ImeDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>

extern "C" void ps4_boot_trace(const char *line);

namespace shell {
namespace {

constexpr uint32_t MAX_TEXT = 255;
char16_t text_buffer[MAX_TEXT + 2];
char16_t title_buffer[128];
bool open = false;

void trace(const char *what, int value) {
    char line[128];
    std::snprintf(line, sizeof line, "keyboard: %s (%08x)", what, static_cast<unsigned>(value));
    std::fprintf(stderr, "shell: %s\n", line);
    ps4_boot_trace(line);
}

// UTF-8 to UTF-16 into out (size including the terminator)
void to_utf16(const std::string &in, char16_t *out, size_t size) {
    size_t n = 0;
    for (size_t i = 0; i < in.size() && n + 2 < size;) {
        const unsigned char c = in[i];
        uint32_t cp = c;
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (extra)
            cp = c & (0x3f >> extra);
        i++;
        for (; extra > 0 && i < in.size(); extra--, i++)
            cp = (cp << 6) | (in[i] & 0x3f);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[n++] = static_cast<char16_t>(0xd800 + (cp >> 10));
            out[n++] = static_cast<char16_t>(0xdc00 + (cp & 0x3ff));
        } else {
            out[n++] = static_cast<char16_t>(cp);
        }
    }
    out[n] = 0;
}

std::string to_utf8(const char16_t *in) {
    std::string out;
    for (size_t i = 0; in[i]; i++) {
        uint32_t cp = in[i];
        if (cp >= 0xd800 && cp < 0xdc00 && in[i + 1] >= 0xdc00 && in[i + 1] < 0xe000)
            cp = 0x10000 + ((cp - 0xd800) << 10) + (in[++i] - 0xdc00);
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }
    return out;
}

} // namespace

bool keyboard_open(const std::string &title, const std::string &text, bool password) {
    if (open)
        return false;
    static bool loaded = false;
    if (!loaded) {
        sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_COMMON_DIALOG);
        sceCommonDialogInitialize();
        loaded = true;
    }
    to_utf16(text, text_buffer, MAX_TEXT + 1);
    to_utf16(title, title_buffer, sizeof(title_buffer) / sizeof(title_buffer[0]));
    int32_t user = -1;
    sceUserServiceGetInitialUser(&user);
    OrbisImeDialogSetting setting{};
    setting.userId = static_cast<uint32_t>(user);
    setting.type = ORBIS_TYPE_DEFAULT;
    setting.option = 0x2 | (password ? 0x4 : 0);  // no automatic capital letters (file names); hidden
    setting.maxTextLength = MAX_TEXT;
    setting.inputTextBuffer = reinterpret_cast<wchar_t *>(text_buffer);
    setting.posx = 1920.f / 2;
    setting.posy = 1080.f / 2;
    setting.horizontalAlignment = ORBIS_H_CENTER;
    setting.verticalAlignment = ORBIS_V_CENTER;
    setting.title = reinterpret_cast<const wchar_t *>(title_buffer);
    const uint32_t option = setting.option;
    int32_t result = sceImeDialogInit(&setting, nullptr);
    if (result < 0) {
        setting.option = password ? 0x4 : 0;  // the plainest
        result = sceImeDialogInit(&setting, nullptr);
    }
    if (result < 0 && hwkeyboard_pause()) {
        // (libSceIme's keyboard was open: tried again without it, which comes back when this closes)
        trace("refused", result);
        setting.option = option;
        if ((result = sceImeDialogInit(&setting, nullptr)) < 0)
            hwkeyboard_dialog_closed();
    }
    trace("opened", result);
    open = result >= 0;
    return open;
}

bool keyboard_active() { return open; }

int keyboard_poll(std::string &text) {
    if (!open)
        return 0;
    const OrbisDialogStatus status = sceImeDialogGetStatus();
    if (status == ORBIS_DIALOG_STATUS_RUNNING)
        return 0;
    OrbisDialogResult result{};
    if (status == ORBIS_DIALOG_STATUS_STOPPED)
        sceImeDialogGetResult(&result);
    sceImeDialogTerm();
    open = false;
    if (status == ORBIS_DIALOG_STATUS_STOPPED && result.endstatus == ORBIS_DIALOG_OK) {
        text = to_utf8(text_buffer);
        return 1;
    }
    return -1;
}

} // namespace shell

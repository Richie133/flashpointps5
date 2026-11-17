// Flashpoint PS5 - the system keyboard (libSceImeDialog) and web browser.
// The keyboard follows ps5-payload-dev/offact's IME_dialog.c (GPL-3.0).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fp/services.hpp"

#include "platform/ps5/system.hpp"

#include <cstdint>
#include <cstring>

namespace
{

// The PS5's wchar_t is 16 bits wide; the structure keeps the SDK's layout.
struct ImeDialogParam
{
    int user_id;
    int type; // 1: basic Latin
    std::uint64_t supported_languages;
    int enter_label; // 2: search
    int input_method;
    void *filter;
    std::uint32_t option;
    std::uint32_t max_text_length;
    std::uint16_t *input_text_buffer;
    float posx;
    float posy;
    int halign; // 1: centre
    int valign; // 1: centre
    const std::uint16_t *placeholder;
    const std::uint16_t *title;
    std::int8_t reserved[16];
};
static_assert(sizeof(ImeDialogParam) == 96, "ImeDialogParam layout");

struct ImeDialogResult
{
    int outcome; // 0 ok, 1 cancelled, 2 aborted
    std::int8_t reserved[12];
};

} // namespace

extern "C"
{
int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(int *);
int sceImeDialogInit(const ImeDialogParam *, void *);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(ImeDialogResult *);
int sceImeDialogTerm(void);
int sceSystemServiceLaunchWebBrowser(const char *uri, void *);
}

namespace hui::fp::services
{

namespace
{
std::uint16_t g_text[512];
std::uint16_t g_title[128];
bool g_open = false;

void to_utf16(const std::string &in, std::uint16_t *out, std::size_t cap)
{
    std::size_t o = 0;
    for (std::size_t i = 0; i < in.size() && o + 1 < cap;)
    {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        std::uint32_t cp = c;
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (extra)
            cp = c & (0x3f >> extra);
        ++i;
        for (; extra > 0 && i < in.size(); --extra, ++i)
            cp = (cp << 6) | (static_cast<unsigned char>(in[i]) & 0x3f);
        out[o++] = cp > 0xffff ? '?' : static_cast<std::uint16_t>(cp);
    }
    out[o] = 0;
}

std::string from_utf16(const std::uint16_t *in)
{
    std::string out;
    for (; *in; ++in)
    {
        const std::uint32_t cp = *in;
        if (cp < 0x80)
        {
            out += static_cast<char>(cp);
        }
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
        else
        {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }
    return out;
}
} // namespace

void init()
{
    const int rc = sceUserServiceInitialize(nullptr);
    sys::log("[FP] user service init %x", rc);
}

bool keyboard_open(const std::string &title, const std::string &text)
{
    if (g_open)
        return true;
    to_utf16(title, g_title, sizeof(g_title) / sizeof(g_title[0]));
    to_utf16(text, g_text, sizeof(g_text) / sizeof(g_text[0]));
    ImeDialogParam param;
    std::memset(&param, 0, sizeof(param));
    param.type = 1;
    param.enter_label = 2;
    param.max_text_length = 100;
    param.input_text_buffer = g_text;
    param.halign = 1;
    param.valign = 1;
    param.posx = 960.0f;
    param.posy = 540.0f;
    param.title = g_title;
    int rc = sceUserServiceGetForegroundUser(&param.user_id);
    if (rc != 0)
    {
        sys::log("[FP] keyboard: no foreground user (%x)", rc);
        return false;
    }
    rc = sceImeDialogInit(&param, nullptr);
    if (rc != 0)
    {
        sys::log("[FP] keyboard: sceImeDialogInit %x", rc);
        return false;
    }
    g_open = true;
    return true;
}

KeyboardState keyboard_poll(std::string *text)
{
    if (!g_open)
        return KeyboardState::idle;
    const int status = sceImeDialogGetStatus();
    if (status == 1)
        return KeyboardState::open;
    if (status != 2)
    {
        g_open = false;
        return KeyboardState::cancelled;
    }
    ImeDialogResult result;
    std::memset(&result, 0, sizeof(result));
    sceImeDialogGetResult(&result);
    sceImeDialogTerm();
    g_open = false;
    if (result.outcome != 0)
        return KeyboardState::cancelled;
    *text = from_utf16(g_text);
    return KeyboardState::done;
}

bool open_browser(const std::string &url)
{
    const int rc = sceSystemServiceLaunchWebBrowser(url.c_str(), nullptr);
    sys::log("[FP] browser %s: %x", url.c_str(), rc);
    return rc == 0;
}

} // namespace hui::fp::services

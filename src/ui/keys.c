/* Copyright 2020 Jaakko Keränen <jaakko.keranen@iki.fi>

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

#include "keys.h"
#include "util.h"
#include "app.h"

#include <the_Foundation/file.h>
#include <the_Foundation/path.h>
#include <the_Foundation/ptrset.h>
#include <SDL_keyboard.h>

enum iModMap {
    none_ModMap,
    leftShift_ModMap,
    leftControl_ModMap,
    leftAlt_ModMap,
    leftGui_ModMap,
    rightShift_ModMap,
    rightControl_ModMap,
    rightAlt_ModMap,
    rightGui_ModMap,
    capsLock_ModMap,
    max_ModMap
};

static const char *modToStr_[max_ModMap] = {
    "none",
    "Lshift",
    "Lctrl",
    "Lalt",
    "Lgui",
    "Rshift",
    "Rctrl",
    "Ralt",
    "Rgui",
    "caps",
};

static int strToMod_(iRangecc str) {
    trim_Rangecc(&str);
    for (int i = 0; i < max_ModMap; i++) {
        if (equalCase_Rangecc(str, modToStr_[i])) {
            return i;
        }
    }
    return none_ModMap;
}

static int modMap_[max_ModMap];
static iBool capsLockDown_;

static void init_ModMap_(void) {
    for (int i = 0; i < max_ModMap; i++) {
        modMap_[i] = i;
    }
}

int mapMods_Keys(int modFlags) {
    static const int bits[max_ModMap] = {
        0,
        KMOD_LSHIFT,
        KMOD_LCTRL,
        KMOD_LALT,
        KMOD_LGUI,
        KMOD_RSHIFT,
        KMOD_RCTRL,
        KMOD_RALT,
        KMOD_RGUI,
        KMOD_CAPS,
    };
    int mapped = 0;
    /* Treat capslock as a modifier key. */
    modFlags |= (capsLockDown_ ? KMOD_CAPS : 0);
    for (int i = 0; i < max_ModMap; ++i) {
        if (modFlags & bits[i]) {
            mapped |= bits[modMap_[i]];
        }
    }
    return mapped;
}

int modState_Keys(void) {
    int state = SDL_GetModState() & ~(KMOD_NUM | KMOD_MODE | KMOD_CAPS);
    /* Treat capslock as a modifier key. */
    if (capsLockDown_) state |= KMOD_CAPS;
    return mapMods_Keys(state);
}

void setCapsLockDown_Keys(iBool isDown) {
    capsLockDown_ = isDown;
}

static void loadModMap_Keys_(const char *saveDir) {
    iFile *f = iClob(newCStr_File(concatPath_CStr(saveDir, "modmap.txt")));
    if (open_File(f, readOnly_FileMode | text_FileMode)) {
        const iString *text = collect_String(readString_File(f));
        iRangecc textLine = iNullRange;
        while (nextSplit_Rangecc(range_String(text), "\n", &textLine)) {
            iRangecc line = textLine;
            trim_Rangecc(&line);
            if (isEmpty_Range(&line) || startsWith_Rangecc(line, "#")) {
                continue; /* comment */
            }
            iRangecc seg = iNullRange;
            if (nextSplit_Rangecc(line, "->", &seg)) {
                const int fromMod = strToMod_(seg);
                if (fromMod && nextSplit_Rangecc(line, "->", &seg)) {
                    const int toMod = strToMod_(seg);
                    modMap_[fromMod] = toMod;
                }
            }
        }
        close_File(f);
    }
    else {
        open_File(f, writeOnly_FileMode | text_FileMode);
        printf_Stream(stream_File(f),
                      "# This is a translation table for keyboard modifiers. The syntax is:\n"
                      "#\n"
                      "# (hardware key) -> (effective modifier)\n"
                      "#\n"
                      "# A modifier can be mapped to \"none\" to disable it. For example:\n"
                      "#\n"
                      "# Lalt -> none\n"
                      "#\n"
                      "# When using CapsLock as a modifier key, its toggled state will still affect\n"
                      "# text entry. You may need to remap or disable CapsLock in your window system.\n"
                      "#\n"
                      "# You may delete this file and it will be recreated with the default mapping.\n\n");
        for (int i = 1; i < max_ModMap; i++) {
            printf_Stream(stream_File(f), "%s -> %s\n", modToStr_[i], modToStr_[i]);
        }
        close_File(f);
    }
}

/*----------------------------------------------------------------------------------------------*/

iDeclareType(Keys)

static int cmpPtr_Binding_(const void *a, const void *b) {
    const iBinding *d = *(const void **) a, *other = *(const void **) b;
    const int cmp = iCmp(d->key, other->key);
    if (cmp == 0) {
        return iCmp(d->mods, other->mods);
    }
    return cmp;
}

/*----------------------------------------------------------------------------------------------*/

struct Impl_Keys {
    iArray  bindings;
    iPtrSet lookup; /* quick key/mods lookup */
};

static iKeys keys_;

static void clear_Keys_(iKeys *d) {
    iForEach(Array, i, &d->bindings) {
        iBinding *bind = i.value;
        deinit_String(&bind->command);
        deinit_String(&bind->label);
    }
}

enum iBindFlag {
    argRepeat_BindFlag  = iBit(1),
    argRelease_BindFlag = iBit(2),
};

/* TODO: This indirection could be used for localization, although all UI strings
   would need to be similarly handled. */
static const struct { int id; iMenuItem bind; int flags; } defaultBindings_[] = {
    { 1,  { "Jump to top",               SDLK_HOME, 0,                  "scroll.top"                        }, 0 },
    { 2,  { "Jump to bottom",            SDLK_END, 0,                   "scroll.bottom"                     }, 0 },
    { 10, { "Scroll up",                 SDLK_UP, 0,                    "scroll.step arg:-1"                }, argRepeat_BindFlag },
    { 11, { "Scroll down",               SDLK_DOWN, 0,                  "scroll.step arg:1"                 }, argRepeat_BindFlag },
    { 20, { "Scroll up half a page",     SDLK_PAGEUP, 0,                "scroll.page arg:-1"                }, argRepeat_BindFlag },
    { 21, { "Scroll down half a page",   SDLK_PAGEDOWN, 0,              "scroll.page arg:1"                 }, argRepeat_BindFlag },
    { 22, { "Scroll up a page",          SDLK_PAGEUP, KMOD_PRIMARY,     "scroll.fullpage arg:-1"            }, argRepeat_BindFlag },
    { 23, { "Scroll down a page",        SDLK_PAGEDOWN, KMOD_PRIMARY,   "scroll.fullpage arg:1"             }, argRepeat_BindFlag },
    { 30, { "Go back",                   navigateBack_KeyShortcut,      "navigate.back"                     }, 0 },
    { 31, { "Go forward",                navigateForward_KeyShortcut,   "navigate.forward"                  }, 0 },
    { 32, { "Go to parent directory",    navigateParent_KeyShortcut,    "navigate.parent"                   }, 0 },
    { 33, { "Go to site root",           navigateRoot_KeyShortcut,      "navigate.root"                     }, 0 },
    { 35, { "Reload page",               reload_KeyShortcut,            "document.reload"                   }, 0 },
    { 41, { "Open link via modifier key", SDLK_LALT, 0,                 "document.linkkeys arg:0"           }, argRelease_BindFlag },
    { 42, { "Open link via home row keys", 'f', 0,                      "document.linkkeys arg:1"           }, 0 },
    { 45, { "Open link in new tab via home row keys", 'f', KMOD_SHIFT,  "document.linkkeys arg:1 newtab:1"  }, 0 },
    { 46, { "Hover on link via home row keys", 'h', 0,                  "document.linkkeys arg:1 hover:1"   }, 0 },
    { 47, { "Next set of home row key links", '.', 0,                   "document.linkkeys more:1"          }, 0 },
    { 50, { "Add bookmark",              'd', KMOD_PRIMARY,             "bookmark.add"                      }, 0 },
    { 60, { "Find text on page",         'f', KMOD_PRIMARY,             "focus.set id:find.input"           }, 0 },
    { 70, { "Zoom in",                   SDLK_EQUALS, KMOD_PRIMARY,     "zoom.delta arg:10"                 }, 0 },
    { 71, { "Zoom out",                  SDLK_MINUS, KMOD_PRIMARY,      "zoom.delta arg:-10"                }, 0 },
    { 72, { "Reset zoom",                SDLK_0, KMOD_PRIMARY,          "zoom.set arg:100"                  }, 0 },
#if !defined (iPlatformApple) /* Ctrl-Cmd-F on macOS */
    { 73, { "Toggle fullscreen mode",    SDLK_F11, 0,                   "window.fullscreen"                 }, 0 },
#endif
    { 76, { "New tab",                   newTab_KeyShortcut,            "tabs.new"                          }, 0 },
    { 77, { "Close tab",                 closeTab_KeyShortcut,          "tabs.close"                        }, 0 },
    { 80, { "Previous tab",              prevTab_KeyShortcut,           "tabs.prev"                         }, 0 },
    { 81, { "Next tab",                  nextTab_KeyShortcut,           "tabs.next"                         }, 0 },
    { 100,{ "Toggle show URL on hover",  '/', KMOD_PRIMARY,             "prefs.hoverlink.toggle"            }, 0 },
    /* The following cannot currently be changed (built-in duplicates). */
    { 1000, { NULL, SDLK_SPACE, KMOD_SHIFT, "scroll.page arg:-1" }, argRepeat_BindFlag },
    { 1001, { NULL, SDLK_SPACE, 0, "scroll.page arg:1" }, argRepeat_BindFlag },
#if defined (iPlatformApple)
    { 1002, { NULL, SDLK_LEFTBRACKET, KMOD_PRIMARY, "navigate.back" }, 0 },
    { 1003, { NULL, SDLK_RIGHTBRACKET, KMOD_PRIMARY, "navigate.forward" }, 0 },
#endif
    { 1004, { NULL, SDLK_F5, 0, "document.reload" }, 0 },
};

static iBinding *findId_Keys_(iKeys *d, int id) {
    iForEach(Array, i, &d->bindings) {
        iBinding *bind = i.value;
        if (bind->id == id) {
            return bind;
        }
    }
    return NULL;
}

static void setFlags_Keys_(int id, int bindFlags) {
    iBinding *bind = findId_Keys_(&keys_, id);
    if (bind) {
        bind->flags = bindFlags;
    }
}

static void bindDefaults_(void) {
    iForIndices(i, defaultBindings_) {
        const int       id   = defaultBindings_[i].id;
        const iMenuItem bind = defaultBindings_[i].bind;
        bind_Keys(id, bind.command, bind.key, bind.kmods);
        if (bind.label) {
            setLabel_Keys(id, bind.label);
        }
        setFlags_Keys_(id, defaultBindings_[i].flags);
    }
}

static iBinding *find_Keys_(iKeys *d, int key, int mods) {
    size_t pos;
    /* Do not differentiate between left and right modifier keys. */
    key = normalizedMod_Sym(key);
    if (isMod_Sym(key)) {
        mods = 0;
    }
    const iBinding elem = { .key = key, .mods = mods };
    if (locate_PtrSet(&d->lookup, &elem, &pos)) {
        return at_PtrSet(&d->lookup, pos);
    }
    return NULL;
}

static iBinding *findCommand_Keys_(iKeys *d, const char *command) {
    /* Note: O(n) */
    iForEach(Array, i, &d->bindings) {
        iBinding *bind = i.value;
        if (!cmp_String(&bind->command, command)) {
            return bind;
        }
    }
    return NULL;
}

static void updateLookup_Keys_(iKeys *d) {
    clear_PtrSet(&d->lookup);
    iConstForEach(Array, i, &d->bindings) {
        insert_PtrSet(&d->lookup, i.value);
    }
}

void setKey_Binding(int id, int key, int mods) {
    iBinding *bind = findId_Keys_(&keys_, id);
    if (bind) {
        bind->key  = normalizedMod_Sym(key);
        bind->mods = isMod_Sym(key) ? 0 : mods;
        updateLookup_Keys_(&keys_);
    }
}

void reset_Binding(int id) {
    iBinding *bind = findId_Keys_(&keys_, id);
    if (bind) {
        iForIndices(i, defaultBindings_) {
            if (defaultBindings_[i].id == id) {
                bind->key  = defaultBindings_[i].bind.key;
                bind->mods = defaultBindings_[i].bind.kmods;
                updateLookup_Keys_(&keys_);
                break;
            }
        }
    }
}

/*----------------------------------------------------------------------------------------------*/

static const char *filename_Keys_ = "bindings.txt";

void init_Keys(void) {
    iKeys *d = &keys_;
    init_ModMap_();
    init_Array(&d->bindings, sizeof(iBinding));
    initCmp_PtrSet(&d->lookup, cmpPtr_Binding_);
    bindDefaults_();
    updateLookup_Keys_(d);
}

void deinit_Keys(void) {
    iKeys *d = &keys_;
    clear_Keys_(d);
    deinit_PtrSet(&d->lookup);
    deinit_Array(&d->bindings);
}

void load_Keys(const char *saveDir) {
    iKeys *d = &keys_;
    loadModMap_Keys_(saveDir);
    iFile *f = iClob(newCStr_File(concatPath_CStr(saveDir, filename_Keys_)));
    if (open_File(f, readOnly_FileMode | text_FileMode)) {
        iBlock * src  = collect_Block(readAll_File(f));
        iRangecc line = iNullRange;
        while (nextSplit_Rangecc(range_Block(src), "\n", &line)) {
            int id, key;
            char modBits[5];
            iZap(modBits);
            sscanf(line.start, "%d %x %4s", &id, &key, modBits);
            iBinding *bind = findId_Keys_(d, id);
            if (bind) {
                bind->key = key;
                bind->mods = 0;
                const char *m = modBits;
                for (int i = 0; i < 4 && *m; i++, m++) {
                    if (*m == 's') bind->mods |= KMOD_SHIFT;
                    if (*m == 'a') bind->mods |= KMOD_ALT;
                    if (*m == 'c') bind->mods |= KMOD_CTRL;
                    if (*m == 'g') bind->mods |= KMOD_GUI;
                    if (*m == 'k') bind->mods |= KMOD_CAPS;
                }
            }
        }
    }
    updateLookup_Keys_(d);
}

void save_Keys(const char *saveDir) {
    iKeys *d = &keys_;
    iFile *f = newCStr_File(concatPath_CStr(saveDir, filename_Keys_));
    if (open_File(f, writeOnly_FileMode | text_FileMode)) {
        iString *line = collectNew_String();
        iConstForEach(Array, i, &d->bindings) {
            const iBinding *bind = i.value;
            format_String(line, "%d %x ", bind->id, bind->key);
            if (bind->mods == 0) {
                appendCStr_String(line, "0");
            }
            else {
                if (bind->mods & KMOD_SHIFT) appendChar_String(line, 's');
                if (bind->mods & KMOD_ALT) appendChar_String(line, 'a');
                if (bind->mods & KMOD_CTRL) appendChar_String(line, 'c');
                if (bind->mods & KMOD_GUI) appendChar_String(line, 'g');
                if (bind->mods & KMOD_CAPS) appendChar_String(line, 'k');
            }
            appendChar_String(line, '\n');
            write_File(f, &line->chars);
        }
        iRelease(f);
    }
}

void bind_Keys(int id, const char *command, int key, int mods) {
    iKeys *d = &keys_;
    iBinding *bind = findId_Keys_(d, id);
    if (!bind) {
        iBinding elem = { .id = id, .key = key, .mods = mods };
        initCStr_String(&elem.command, command);
        init_String(&elem.label);
        pushBack_Array(&d->bindings, &elem);
    }
    else {
        setCStr_String(&bind->command, command);
        bind->key  = key;
        bind->mods = mods;
    }
}

void setLabel_Keys(int id, const char *label) {
    iBinding *bind = findId_Keys_(&keys_, id);
    if (bind) {
        setCStr_String(&bind->label, label);
    }
}

iBool processEvent_Keys(const SDL_Event *ev) {
    iKeys *d = &keys_;
    if (ev->type == SDL_KEYDOWN || ev->type == SDL_KEYUP) {
        const iBinding *bind = find_Keys_(d, ev->key.keysym.sym, keyMods_Sym(ev->key.keysym.mod));
        if (bind) {
            if (ev->type == SDL_KEYUP) {
                if (bind->flags & argRelease_BindFlag) {
                    postCommandf_App("%s release:1", cstr_String(&bind->command));
                    return iTrue;
                }
                return iFalse;
            }
            if (ev->key.repeat && (bind->flags & argRepeat_BindFlag)) {
                postCommandf_App("%s repeat:1", cstr_String(&bind->command));
            }
            else {
                postCommandString_App(&bind->command);
            }
            return iTrue;
        }
    }
    return iFalse;
}

const iBinding *findCommand_Keys(const char *command) {
    return findCommand_Keys_(&keys_, command);
}

const iPtrArray *list_Keys(void) {
    iKeys *d = &keys_;
    iPtrArray *list = collectNew_PtrArray();
    iConstForEach(Array, i, &d->bindings) {
        pushBack_PtrArray(list, i.value);
    }
    return list;
}

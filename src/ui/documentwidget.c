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

/* TODO: This file is a little too large. DocumentWidget could be split into
   a couple of smaller objects. One for rendering the document, for instance. */

#include "documentwidget.h"

#include "app.h"
#include "audio/player.h"
#include "bookmarks.h"
#include "command.h"
#include "defs.h"
#include "gmcerts.h"
#include "gmdocument.h"
#include "gmrequest.h"
#include "gmutil.h"
#include "history.h"
#include "indicatorwidget.h"
#include "inputwidget.h"
#include "keys.h"
#include "labelwidget.h"
#include "media.h"
#include "paint.h"
#include "mediaui.h"
#include "scrollwidget.h"
#include "util.h"
#include "visbuf.h"
#include "visited.h"

#include <the_Foundation/file.h>
#include <the_Foundation/fileinfo.h>
#include <the_Foundation/objectlist.h>
#include <the_Foundation/path.h>
#include <the_Foundation/ptrarray.h>
#include <the_Foundation/ptrset.h>
#include <the_Foundation/regexp.h>
#include <the_Foundation/stringarray.h>
#include <SDL_clipboard.h>
#include <SDL_timer.h>
#include <SDL_render.h>
#include <ctype.h>
#include <errno.h>

/*----------------------------------------------------------------------------------------------*/

iDeclareType(PersistentDocumentState)
iDeclareTypeConstruction(PersistentDocumentState)
iDeclareTypeSerialization(PersistentDocumentState)

enum iReloadInterval {
    never_RelodPeriod,
    minute_ReloadInterval,
    fiveMinutes_ReloadInterval,
    fifteenMinutes_ReloadInterval,
    hour_ReloadInterval,
    fourHours_ReloadInterval,
    twicePerDay_ReloadInterval,
    day_ReloadInterval,
    max_ReloadInterval
};

static int seconds_ReloadInterval_(enum iReloadInterval d) {
    static const int times[] = { 0, 1, 5, 15, 60, 4 * 60, 12 * 60, 24 * 60 };
    if (d < 0 || d >= max_ReloadInterval) return 0;
    return times[d];
}

static const char *label_ReloadInterval_(enum iReloadInterval d) {
    static const char *labels[] = {
        "Never",
        "1 minute",
        "5 minutes",
        "15 minutes",
        "1 hour",
        "4 hours",
        "12 hours",
        "Once per day"
    };
    if (d < 0 || d >= max_ReloadInterval) return 0;
    return labels[d];
}

struct Impl_PersistentDocumentState {
    iHistory *history;
    iString * url;
    enum iReloadInterval reloadInterval;
};

void init_PersistentDocumentState(iPersistentDocumentState *d) {
    d->history        = new_History();
    d->url            = new_String();
    d->reloadInterval = 0;
}

void deinit_PersistentDocumentState(iPersistentDocumentState *d) {
    delete_String(d->url);
    delete_History(d->history);
}

void serialize_PersistentDocumentState(const iPersistentDocumentState *d, iStream *outs) {
    serialize_String(d->url, outs);
    writeU16_Stream(outs, d->reloadInterval & 7);
    serialize_History(d->history, outs);
}

void deserialize_PersistentDocumentState(iPersistentDocumentState *d, iStream *ins) {
    deserialize_String(d->url, ins);
    if (indexOfCStr_String(d->url, " ptr:0x") != iInvalidPos) {
        /* Oopsie, this should not have been written; invalid URL. */
        clear_String(d->url);
    }
    const uint16_t params = readU16_Stream(ins);
    d->reloadInterval = params & 7;
    deserialize_History(d->history, ins);
}

iDefineTypeConstruction(PersistentDocumentState)

/*----------------------------------------------------------------------------------------------*/

static void animateMedia_DocumentWidget_      (iDocumentWidget *d);
static void updateSideIconBuf_DocumentWidget_   (iDocumentWidget *d);

static const int smoothDuration_DocumentWidget_  = 600; /* milliseconds */
static const int outlineMinWidth_DocumentWdiget_ = 45;  /* times gap_UI */
static const int outlineMaxWidth_DocumentWidget_ = 65;  /* times gap_UI */
static const int outlinePadding_DocumentWidget_  = 3;   /* times gap_UI */

enum iRequestState {
    blank_RequestState,
    fetching_RequestState,
    receivedPartialResponse_RequestState,
    ready_RequestState,
};

enum iDocumentWidgetFlag {
    selecting_DocumentWidgetFlag             = iBit(1),
    noHoverWhileScrolling_DocumentWidgetFlag = iBit(2),
    showLinkNumbers_DocumentWidgetFlag       = iBit(3),
    setHoverViaKeys_DocumentWidgetFlag       = iBit(4),
    newTabViaHomeKeys_DocumentWidgetFlag     = iBit(5),
    centerVertically_DocumentWidgetFlag      = iBit(6),
};

enum iDocumentLinkOrdinalMode {
    numbersAndAlphabet_DocumentLinkOrdinalMode,
    homeRow_DocumentLinkOrdinalMode,
};

struct Impl_DocumentWidget {
    iWidget        widget;
    enum iRequestState state;
    iPersistentDocumentState mod;
    int            flags;
    enum iDocumentLinkOrdinalMode ordinalMode;
    size_t         ordinalBase;
    iString *      titleUser;
    iGmRequest *   request;
    iAtomicInt     isRequestUpdated; /* request has new content, need to parse it */
    iObjectList *  media;
    enum iGmStatusCode sourceStatus;
    iString        sourceHeader;
    iString        sourceMime;
    iBlock         sourceContent; /* original content as received, for saving */
    iTime          sourceTime;
    iGmDocument *  doc;
    int            certFlags;
    iBlock *       certFingerprint;
    iDate          certExpiry;
    iString *      certSubject;
    int            redirectCount;
    iRangecc       selectMark;
    iRangecc       foundMark;
    int            pageMargin;
    iPtrArray      visibleLinks;
    iPtrArray      visibleWideRuns; /* scrollable blocks */
    iArray         wideRunOffsets;
    iAnim          animWideRunOffset;
    uint16_t       animWideRunId;
    iGmRunRange    animWideRunRange;
    iPtrArray      visibleMedia; /* currently playing audio / ongoing downloads */
    const iGmRun * grabbedPlayer; /* currently adjusting volume in a player */
    float          grabbedStartVolume;
    int            mediaTimer;
    const iGmRun * hoverLink;
    const iGmRun * contextLink;
    const iGmRun * firstVisibleRun;
    const iGmRun * lastVisibleRun;
    iClick         click;
    iString        pendingGotoHeading;
    float          initNormScrollY;
    iAnim          scrollY;
    iAnim          sideOpacity;
    iScrollWidget *scroll;
    iWidget *      menu;
    iWidget *      playerMenu;
    iVisBuf *      visBuf;
    iPtrSet *      invalidRuns;
    SDL_Texture *  sideIconBuf;
    iTextBuf *     timestampBuf;
};

iDefineObjectConstruction(DocumentWidget)

void init_DocumentWidget(iDocumentWidget *d) {
    iWidget *w = as_Widget(d);
    init_Widget(w);
    setId_Widget(w, "document000");
    setFlags_Widget(w, hover_WidgetFlag, iTrue);
    init_PersistentDocumentState(&d->mod);
    d->flags = 0;
    iZap(d->certExpiry);
    d->certFingerprint  = new_Block(0);
    d->certFlags        = 0;
    d->certSubject      = new_String();
    d->state            = blank_RequestState;
    d->titleUser        = new_String();
    d->request          = NULL;
    d->isRequestUpdated = iFalse;
    d->media            = new_ObjectList();
    d->doc              = new_GmDocument();
    d->redirectCount    = 0;
    d->ordinalBase      = 0;
    d->initNormScrollY  = 0;
    init_Anim(&d->scrollY, 0);
    d->animWideRunId = 0;
    init_Anim(&d->animWideRunOffset, 0);
    d->selectMark       = iNullRange;
    d->foundMark        = iNullRange;
    d->pageMargin       = 5;
    d->hoverLink        = NULL;
    d->contextLink      = NULL;
    d->firstVisibleRun  = NULL;
    d->lastVisibleRun   = NULL;
    d->visBuf           = new_VisBuf();
    d->invalidRuns      = new_PtrSet();
    init_Anim(&d->sideOpacity, 0);
    d->sourceStatus = none_GmStatusCode;
    init_String(&d->sourceHeader);
    init_String(&d->sourceMime);
    init_Block(&d->sourceContent, 0);
    iZap(d->sourceTime);
    init_PtrArray(&d->visibleLinks);
    init_PtrArray(&d->visibleWideRuns);
    init_Array(&d->wideRunOffsets, sizeof(int));
    init_PtrArray(&d->visibleMedia);
    d->grabbedPlayer = NULL;
    d->mediaTimer    = 0;
    init_String(&d->pendingGotoHeading);
    init_Click(&d->click, d, SDL_BUTTON_LEFT);
    addChild_Widget(w, iClob(d->scroll = new_ScrollWidget()));
    d->menu         = NULL; /* created when clicking */
    d->playerMenu   = NULL;
    d->sideIconBuf  = NULL;
    d->timestampBuf = NULL;
    addChildFlags_Widget(w,
                         iClob(new_IndicatorWidget()),
                         resizeToParentWidth_WidgetFlag | resizeToParentHeight_WidgetFlag);
#if !defined (iPlatformAppleDesktop) /* in system menu */
    addAction_Widget(w, reload_KeyShortcut, "navigate.reload");
    addAction_Widget(w, closeTab_KeyShortcut, "tabs.close");
    addAction_Widget(w, SDLK_d, KMOD_PRIMARY, "bookmark.add");
    addAction_Widget(w, subscribeToPage_KeyModifier, "feeds.subscribe");
#endif
    addAction_Widget(w, navigateBack_KeyShortcut, "navigate.back");
    addAction_Widget(w, navigateForward_KeyShortcut, "navigate.forward");
    addAction_Widget(w, navigateParent_KeyShortcut, "navigate.parent");
    addAction_Widget(w, navigateRoot_KeyShortcut, "navigate.root");
}

void deinit_DocumentWidget(iDocumentWidget *d) {
    if (d->sideIconBuf) {
        SDL_DestroyTexture(d->sideIconBuf);
    }
    delete_TextBuf(d->timestampBuf);
    delete_VisBuf(d->visBuf);
    delete_PtrSet(d->invalidRuns);
    iRelease(d->media);
    iRelease(d->request);
    deinit_String(&d->pendingGotoHeading);
    deinit_Block(&d->sourceContent);
    deinit_String(&d->sourceMime);
    deinit_String(&d->sourceHeader);
    iRelease(d->doc);
    if (d->mediaTimer) {
        SDL_RemoveTimer(d->mediaTimer);
    }
    deinit_Array(&d->wideRunOffsets);
    deinit_PtrArray(&d->visibleMedia);
    deinit_PtrArray(&d->visibleWideRuns);
    deinit_PtrArray(&d->visibleLinks);
    delete_Block(d->certFingerprint);
    delete_String(d->certSubject);
    delete_String(d->titleUser);
    deinit_PersistentDocumentState(&d->mod);
}

static void resetWideRuns_DocumentWidget_(iDocumentWidget *d) {
    clear_Array(&d->wideRunOffsets);
    d->animWideRunId = 0;
    init_Anim(&d->animWideRunOffset, 0);
    iZap(d->animWideRunRange);
}

static void requestUpdated_DocumentWidget_(iAnyObject *obj) {
    iDocumentWidget *d = obj;
    const int wasUpdated = exchange_Atomic(&d->isRequestUpdated, iTrue);
    if (!wasUpdated) {
        postCommand_Widget(obj, "document.request.updated doc:%p request:%p", d, d->request);
    }
}

static void requestFinished_DocumentWidget_(iAnyObject *obj) {
    iDocumentWidget *d = obj;
    postCommand_Widget(obj, "document.request.finished doc:%p request:%p", d, d->request);
}

static int documentWidth_DocumentWidget_(const iDocumentWidget *d) {
    const iWidget *w        = constAs_Widget(d);
    const iRect    bounds   = bounds_Widget(w);
    const iPrefs * prefs    = prefs_App();
    const int      minWidth = 50 * gap_UI; /* lines must fit a word at least */
    const float    adjust   = iClamp((float) bounds.size.x / gap_UI / 11 - 12,
                                     -2.0f, 10.0f); /* adapt to width */
    //printf("%f\n", adjust); fflush(stdout);
    return iMini(iMax(minWidth, bounds.size.x - gap_UI * (d->pageMargin + adjust) * 2),
                 fontSize_UI * prefs->lineWidth * prefs->zoomPercent / 100);
}

static iRect documentBounds_DocumentWidget_(const iDocumentWidget *d) {
    const iRect bounds = bounds_Widget(constAs_Widget(d));
    const int   margin = gap_UI * d->pageMargin;
    iRect       rect;
    rect.size.x = documentWidth_DocumentWidget_(d);
    rect.pos.x  = mid_Rect(bounds).x - rect.size.x / 2;
    rect.pos.y  = top_Rect(bounds);
    rect.size.y = height_Rect(bounds) - margin;
    const iGmRun *banner = siteBanner_GmDocument(d->doc);
    if (!banner) {
        rect.pos.y += margin;
        rect.size.y -= margin;
    }
    if (d->flags & centerVertically_DocumentWidgetFlag) {
        const iInt2 docSize = size_GmDocument(d->doc);
        if (docSize.y < rect.size.y) {
            /* Center vertically if short. There is one empty paragraph line's worth of margin
               between the banner and the page contents. */
            const int bannerHeight = banner ? height_Rect(banner->visBounds) : 0;
            int offset = iMax(0, (rect.size.y + margin - docSize.y - bannerHeight -
                                  lineHeight_Text(paragraph_FontId)) / 2);
            rect.pos.y += offset;
            rect.size.y = docSize.y;
        }
    }
    return rect;
}

static iRect siteBannerRect_DocumentWidget_(const iDocumentWidget *d) {
    const iGmRun *banner = siteBanner_GmDocument(d->doc);
    if (!banner) {
        return zero_Rect();
    }
    const iRect docBounds = documentBounds_DocumentWidget_(d);
    const iInt2 origin = addY_I2(topLeft_Rect(docBounds), -value_Anim(&d->scrollY));
    return moved_Rect(banner->visBounds, origin);
}

static iInt2 documentPos_DocumentWidget_(const iDocumentWidget *d, iInt2 pos) {
    return addY_I2(sub_I2(pos, topLeft_Rect(documentBounds_DocumentWidget_(d))),
                   value_Anim(&d->scrollY));
}

static iRangei visibleRange_DocumentWidget_(const iDocumentWidget *d) {
    const int margin = !hasSiteBanner_GmDocument(d->doc) ? gap_UI * d->pageMargin : 0;
    return (iRangei){ value_Anim(&d->scrollY) - margin,
                      value_Anim(&d->scrollY) + height_Rect(bounds_Widget(constAs_Widget(d))) -
                          margin };
}

static void addVisible_DocumentWidget_(void *context, const iGmRun *run) {
    iDocumentWidget *d = context;
    if (~run->flags & decoration_GmRunFlag && !run->mediaId) {
        if (!d->firstVisibleRun) {
            d->firstVisibleRun = run;
        }
        d->lastVisibleRun = run;
    }
    if (run->preId && run->flags & wide_GmRunFlag) {
        pushBack_PtrArray(&d->visibleWideRuns, run);
    }
    if (run->mediaType == audio_GmRunMediaType || run->mediaType == download_GmRunMediaType) {
        iAssert(run->mediaId);
        pushBack_PtrArray(&d->visibleMedia, run);
    }
    if (run->linkId) {
        pushBack_PtrArray(&d->visibleLinks, run);
    }
}

static const iGmRun *lastVisibleLink_DocumentWidget_(const iDocumentWidget *d) {
    iReverseConstForEach(PtrArray, i, &d->visibleLinks) {
        const iGmRun *run = i.ptr;
        if (run->flags & decoration_GmRunFlag && run->linkId) {
            return run;
        }
    }
    return NULL;
}

static float normScrollPos_DocumentWidget_(const iDocumentWidget *d) {
    const int docSize = size_GmDocument(d->doc).y;
    if (docSize) {
        return value_Anim(&d->scrollY) / (float) docSize;
    }
    return 0;
}

static int scrollMax_DocumentWidget_(const iDocumentWidget *d) {
    return size_GmDocument(d->doc).y - height_Rect(bounds_Widget(constAs_Widget(d))) +
           (hasSiteBanner_GmDocument(d->doc) ? 1 : 2) * d->pageMargin * gap_UI;
}

static void invalidateLink_DocumentWidget_(iDocumentWidget *d, iGmLinkId id) {
    /* A link has multiple runs associated with it. */
    iConstForEach(PtrArray, i, &d->visibleLinks) {
        const iGmRun *run = i.ptr;
        if (run->linkId == id) {
            insert_PtrSet(d->invalidRuns, run);
        }
    }
}

static void invalidateVisibleLinks_DocumentWidget_(iDocumentWidget *d) {
    iConstForEach(PtrArray, i, &d->visibleLinks) {
        const iGmRun *run = i.ptr;
        if (run->linkId) {
            insert_PtrSet(d->invalidRuns, run);
        }
    }
}

static int runOffset_DocumentWidget_(const iDocumentWidget *d, const iGmRun *run) {
    if (run->preId && run->flags & wide_GmRunFlag) {
        if (d->animWideRunId == run->preId) {
            return -value_Anim(&d->animWideRunOffset);
        }
        const size_t numOffsets = size_Array(&d->wideRunOffsets);
        const int *offsets = constData_Array(&d->wideRunOffsets);
        if (run->preId <= numOffsets) {
            return -offsets[run->preId - 1];
        }
    }
    return 0;
}

static void invalidateWideRunsWithNonzeroOffset_DocumentWidget_(iDocumentWidget *d) {
    iConstForEach(PtrArray, i, &d->visibleWideRuns) {
        const iGmRun *run = i.ptr;
        if (runOffset_DocumentWidget_(d, run)) {
            insert_PtrSet(d->invalidRuns, run);
        }
    }
}

static void updateHover_DocumentWidget_(iDocumentWidget *d, iInt2 mouse) {
    const iWidget *w            = constAs_Widget(d);
    const iRect    docBounds    = documentBounds_DocumentWidget_(d);
    const iGmRun * oldHoverLink = d->hoverLink;
    d->hoverLink                = NULL;
    const iInt2 hoverPos = addY_I2(sub_I2(mouse, topLeft_Rect(docBounds)), value_Anim(&d->scrollY));
    if (isHover_Widget(w) && (~d->flags & noHoverWhileScrolling_DocumentWidgetFlag) &&
        (d->state == ready_RequestState || d->state == receivedPartialResponse_RequestState)) {
        iConstForEach(PtrArray, i, &d->visibleLinks) {
            const iGmRun *run = i.ptr;
            /* Click targets are slightly expanded so there are no gaps between links. */
            if (contains_Rect(expanded_Rect(run->bounds, init1_I2(gap_Text / 2)), hoverPos)) {
                d->hoverLink = run;
                break;
            }
        }
    }
    if (d->hoverLink != oldHoverLink) {
        if (oldHoverLink) {
            invalidateLink_DocumentWidget_(d, oldHoverLink->linkId);
        }
        if (d->hoverLink) {
            invalidateLink_DocumentWidget_(d, d->hoverLink->linkId);
        }
        refresh_Widget(as_Widget(d));
    }
    if (isHover_Widget(w) && !contains_Widget(constAs_Widget(d->scroll), mouse)) {
        setCursor_Window(get_Window(),
                         d->hoverLink ? SDL_SYSTEM_CURSOR_HAND : SDL_SYSTEM_CURSOR_IBEAM);
        if (d->hoverLink &&
            linkFlags_GmDocument(d->doc, d->hoverLink->linkId) & permanent_GmLinkFlag) {
            setCursor_Window(get_Window(), SDL_SYSTEM_CURSOR_ARROW); /* not dismissable */
        }
    }
}

static void animate_DocumentWidget_(void *ticker) {
    iDocumentWidget *d = ticker;
    if (!isFinished_Anim(&d->sideOpacity)) {
        addTicker_App(animate_DocumentWidget_, d);
    }
}

static void updateSideOpacity_DocumentWidget_(iDocumentWidget *d, iBool isAnimated) {
    float opacity = 0.0f;
    const iGmRun *banner = siteBanner_GmDocument(d->doc);
    if (banner && bottom_Rect(banner->visBounds) < value_Anim(&d->scrollY)) {
        opacity = 1.0f;
    }
    setValue_Anim(&d->sideOpacity, opacity, isAnimated ? (opacity < 0.5f ? 100 : 200) : 0);
    animate_DocumentWidget_(d);
}

static uint32_t mediaUpdateInterval_DocumentWidget_(const iDocumentWidget *d) {
    if (document_App() != d) {
        return 0;
    }
    static const uint32_t invalidInterval_ = ~0u;
    uint32_t interval = invalidInterval_;
    iConstForEach(PtrArray, i, &d->visibleMedia) {
        const iGmRun *run = i.ptr;
        if (run->mediaType == audio_GmRunMediaType) {
            iPlayer *plr = audioPlayer_Media(media_GmDocument(d->doc), run->mediaId);
            if (flags_Player(plr) & adjustingVolume_PlayerFlag ||
                (isStarted_Player(plr) && !isPaused_Player(plr))) {
                interval = iMin(interval, 1000 / 15);
            }
        }
        else if (run->mediaType == download_GmRunMediaType) {
            interval = iMin(interval, 1000);
        }
    }
    return interval != invalidInterval_ ? interval : 0;
}

static uint32_t postMediaUpdate_DocumentWidget_(uint32_t interval, void *context) {
    /* Called in timer thread; don't access the widget. */
    iUnused(context);
    postCommand_App("media.player.update");
    return interval;
}

static void updateMedia_DocumentWidget_(iDocumentWidget *d) {
    if (document_App() == d) {
        refresh_Widget(d);
        iConstForEach(PtrArray, i, &d->visibleMedia) {
            const iGmRun *run = i.ptr;
            if (run->mediaType == audio_GmRunMediaType) {
                iPlayer *plr = audioPlayer_Media(media_GmDocument(d->doc), run->mediaId);
                if (idleTimeMs_Player(plr) > 3000 && ~flags_Player(plr) & volumeGrabbed_PlayerFlag &&
                    flags_Player(plr) & adjustingVolume_PlayerFlag) {
                    setFlags_Player(plr, adjustingVolume_PlayerFlag, iFalse);
                }
            }
        }
    }
    if (d->mediaTimer && mediaUpdateInterval_DocumentWidget_(d) == 0) {
        SDL_RemoveTimer(d->mediaTimer);
        d->mediaTimer = 0;
    }
}

static void animateMedia_DocumentWidget_(iDocumentWidget *d) {
    if (document_App() != d) {
        if (d->mediaTimer) {
            SDL_RemoveTimer(d->mediaTimer);
            d->mediaTimer = 0;
        }
        return;
    }
    uint32_t interval = mediaUpdateInterval_DocumentWidget_(d);
    if (interval && !d->mediaTimer) {
        d->mediaTimer = SDL_AddTimer(interval, postMediaUpdate_DocumentWidget_, d);
    }
}

static iRangecc currentHeading_DocumentWidget_(const iDocumentWidget *d) {
    iRangecc heading = iNullRange;
    if (d->firstVisibleRun) {
        iConstForEach(Array, i, headings_GmDocument(d->doc)) {
            const iGmHeading *head = i.value;
            if (head->level == 0) {
                if (head->text.start <= d->firstVisibleRun->text.start) {
                    heading = head->text;
                }
                if (d->lastVisibleRun && head->text.start > d->lastVisibleRun->text.start) {
                    break;
                }
            }
        }
    }
    return heading;
}

static void updateVisible_DocumentWidget_(iDocumentWidget *d) {
    iChangeFlags(d->flags,
                 centerVertically_DocumentWidgetFlag,
                 prefs_App()->centerShortDocs || startsWithCase_String(d->mod.url, "about:") ||
                     !isSuccess_GmStatusCode(d->sourceStatus));
    const iRangei visRange = visibleRange_DocumentWidget_(d);
    const iRect   bounds   = bounds_Widget(as_Widget(d));
    setRange_ScrollWidget(d->scroll, (iRangei){ 0, scrollMax_DocumentWidget_(d) });
    const int docSize = size_GmDocument(d->doc).y;
    setThumb_ScrollWidget(d->scroll,
                          value_Anim(&d->scrollY),
                          docSize > 0 ? height_Rect(bounds) * size_Range(&visRange) / docSize : 0);
    clear_PtrArray(&d->visibleLinks);
    clear_PtrArray(&d->visibleWideRuns);
    clear_PtrArray(&d->visibleMedia);
    const iRangecc oldHeading = currentHeading_DocumentWidget_(d);
    /* Scan for visible runs. */ {
        d->firstVisibleRun = NULL;
        render_GmDocument(d->doc, visRange, addVisible_DocumentWidget_, d);
    }
    const iRangecc newHeading = currentHeading_DocumentWidget_(d);
    if (memcmp(&oldHeading, &newHeading, sizeof(oldHeading))) {
        updateSideIconBuf_DocumentWidget_(d);
    }
    updateHover_DocumentWidget_(d, mouseCoord_Window(get_Window()));
    updateSideOpacity_DocumentWidget_(d, iTrue);
    animateMedia_DocumentWidget_(d);
    /* Remember scroll positions of recently visited pages. */ {
        iRecentUrl *recent = mostRecentUrl_History(d->mod.history);
        if (recent && docSize && d->state == ready_RequestState) {
            recent->normScrollY = normScrollPos_DocumentWidget_(d);
        }
    }
}

static void updateWindowTitle_DocumentWidget_(const iDocumentWidget *d) {
    iLabelWidget *tabButton = tabPageButton_Widget(findWidget_App("doctabs"), d);
    if (!tabButton) {
        /* Not part of the UI at the moment. */
        return;
    }
    iStringArray *title = iClob(new_StringArray());
    if (!isEmpty_String(title_GmDocument(d->doc))) {
        pushBack_StringArray(title, title_GmDocument(d->doc));
    }
    if (!isEmpty_String(d->titleUser)) {
        pushBack_StringArray(title, d->titleUser);
    }
    else {
        iUrl parts;
        init_Url(&parts, d->mod.url);
        if (equalCase_Rangecc(parts.scheme, "about")) {
            if (!findWidget_App("winbar")) {
                pushBackCStr_StringArray(title, "Lagrange");
            }
        }
        else if (!isEmpty_Range(&parts.host)) {
            pushBackRange_StringArray(title, parts.host);
        }
    }
    if (isEmpty_StringArray(title)) {
        pushBackCStr_StringArray(title, "Lagrange");
    }
    /* Take away parts if it doesn't fit. */
    const int avail = bounds_Widget(as_Widget(tabButton)).size.x - 3 * gap_UI;
    iBool setWindow = (document_App() == d);
    for (;;) {
        iString *text = collect_String(joinCStr_StringArray(title, " \u2014 "));
        if (setWindow) {
            /* Longest version for the window title, and omit the icon. */
            setTitle_Window(get_Window(), text);
            setWindow = iFalse;
        }
        const iChar siteIcon = siteIcon_GmDocument(d->doc);
        if (siteIcon) {
            if (!isEmpty_String(text)) {
                prependCStr_String(text, "  " restore_ColorEscape);
            }
            prependChar_String(text, siteIcon);
            prependCStr_String(text, escape_Color(uiIcon_ColorId));
        }
        const int width = advanceRange_Text(default_FontId, range_String(text)).x;
        if (width <= avail ||
            isEmpty_StringArray(title)) {
            updateText_LabelWidget(tabButton, text);
            break;
        }
        if (size_StringArray(title) == 1) {
            /* Just truncate to fit. */
            const char *endPos;
            tryAdvanceNoWrap_Text(default_FontId,
                                  range_String(text),
                                  avail - advance_Text(default_FontId, "...").x,
                                  &endPos);
            updateText_LabelWidget(
                tabButton,
                collectNewFormat_String(
                    "%s...", cstr_Rangecc((iRangecc){ constBegin_String(text), endPos })));
            break;
        }
        remove_StringArray(title, size_StringArray(title) - 1);
    }
}

static void updateTimestampBuf_DocumentWidget_(iDocumentWidget *d) {
    if (d->timestampBuf) {
        delete_TextBuf(d->timestampBuf);
        d->timestampBuf = NULL;
    }
    if (isValid_Time(&d->sourceTime)) {
        d->timestampBuf = new_TextBuf(
            uiLabel_FontId,
            cstrCollect_String(format_Time(&d->sourceTime, "Received at %I:%M %p\non %b %d, %Y")));
    }
}

static void invalidate_DocumentWidget_(iDocumentWidget *d) {
    invalidate_VisBuf(d->visBuf);
    clear_PtrSet(d->invalidRuns);
}

static iRangecc bannerText_DocumentWidget_(const iDocumentWidget *d) {
    return isEmpty_String(d->titleUser) ? range_String(bannerText_GmDocument(d->doc))
                                        : range_String(d->titleUser);
}

static void documentRunsInvalidated_DocumentWidget_(iDocumentWidget *d) {
    d->foundMark       = iNullRange;
    d->selectMark      = iNullRange;
    d->hoverLink       = NULL;
    d->contextLink     = NULL;
    d->firstVisibleRun = NULL;
    d->lastVisibleRun  = NULL;
}

static void setSource_DocumentWidget_(iDocumentWidget *d, const iString *source) {
    setUrl_GmDocument(d->doc, d->mod.url);
    setSource_GmDocument(d->doc, source, documentWidth_DocumentWidget_(d));
    documentRunsInvalidated_DocumentWidget_(d);
    updateWindowTitle_DocumentWidget_(d);
    updateVisible_DocumentWidget_(d);
    updateSideIconBuf_DocumentWidget_(d);
    invalidate_DocumentWidget_(d);
    refresh_Widget(as_Widget(d));
}

static void updateTheme_DocumentWidget_(iDocumentWidget *d) {
    if (isEmpty_String(d->titleUser)) {
        setThemeSeed_GmDocument(d->doc,
                                collect_Block(newRange_Block(urlHost_String(d->mod.url))));
    }
    else {
        setThemeSeed_GmDocument(d->doc, &d->titleUser->chars);
    }
    updateTimestampBuf_DocumentWidget_(d);
}

static enum iGmDocumentBanner bannerType_DocumentWidget_(const iDocumentWidget *d) {
    if (d->certFlags & available_GmCertFlag) {
        const int req = domainVerified_GmCertFlag | timeVerified_GmCertFlag | trusted_GmCertFlag;
        if ((d->certFlags & req) != req) {
            return certificateWarning_GmDocumentBanner;
        }
    }
    return siteDomain_GmDocumentBanner;
}

static void showErrorPage_DocumentWidget_(iDocumentWidget *d, enum iGmStatusCode code,
                                          const iString *meta) {
    iString *src = collectNewCStr_String("# ");
    const iGmError *msg = get_GmError(code);
    appendChar_String(src, msg->icon ? msg->icon : 0x2327); /* X in a box */
    appendFormat_String(src, " %s\n%s", msg->title, msg->info);
    iBool useBanner = iTrue;
    if (meta) {
        switch (code) {
            case schemeChangeRedirect_GmStatusCode:
            case tooManyRedirects_GmStatusCode:
                appendFormat_String(src, "\n=> %s\n", cstr_String(meta));
                break;
            case tlsFailure_GmStatusCode:
                useBanner = iFalse; /* valid data wasn't received from host */
                appendFormat_String(src, "\n\n>%s\n", cstr_String(meta));
                break;
            case failedToOpenFile_GmStatusCode:
            case certificateNotValid_GmStatusCode:
                appendFormat_String(src, "\n\n%s", cstr_String(meta));
                break;
            case unsupportedMimeType_GmStatusCode: {
                iString *key = collectNew_String();
                toString_Sym(SDLK_s, KMOD_PRIMARY, key);
                appendFormat_String(src,
                                    "\n```\n%s\n```\n"
                                    "You can save it as a file to your Downloads folder, though. "
                                    "Press %s or select \"Save to Downloads\" from the menu.",
                                    cstr_String(meta),
                                    cstr_String(key));
                break;
            }
            case slowDown_GmStatusCode:
                appendFormat_String(src, "\n\nWait %s seconds before your next request.",
                                    cstr_String(meta));
                break;
            default:
                break;
        }
    }
    setBanner_GmDocument(d->doc, useBanner ? bannerType_DocumentWidget_(d) : none_GmDocumentBanner);
    setFormat_GmDocument(d->doc, gemini_GmDocumentFormat);
    setSource_DocumentWidget_(d, src);
    updateTheme_DocumentWidget_(d);
    init_Anim(&d->scrollY, 0);
    init_Anim(&d->sideOpacity, 0);
    resetWideRuns_DocumentWidget_(d);
    d->state = ready_RequestState;
}

static void updateFetchProgress_DocumentWidget_(iDocumentWidget *d) {
    iLabelWidget *prog   = findWidget_App("document.progress");
    const size_t  dlSize = d->request ? bodySize_GmRequest(d->request) : 0;
    showCollapsed_Widget(as_Widget(prog), dlSize >= 250000);
    if (isVisible_Widget(prog)) {
        updateText_LabelWidget(prog,
                               collectNewFormat_String("%s%.3f MB",
                                                       isFinished_GmRequest(d->request)
                                                           ? uiHeading_ColorEscape
                                                           : uiTextCaution_ColorEscape,
                                                       dlSize / 1.0e6f));
    }
}

static void updateDocument_DocumentWidget_(iDocumentWidget *d, const iGmResponse *response,
                                           const iBool isInitialUpdate) {
    if (d->state == ready_RequestState) {
        return;
    }
    const iBool isRequestFinished = !d->request || isFinished_GmRequest(d->request);
    /* TODO: Do document update in the background. However, that requires a text metrics calculator
       that does not try to cache the glyph bitmaps. */
    const enum iGmStatusCode statusCode = response->statusCode;
    if (category_GmStatusCode(statusCode) != categoryInput_GmStatusCode) {
        iBool setSource = iTrue;
        iString str;
        invalidate_DocumentWidget_(d);
        if (document_App() == d) {
            updateTheme_DocumentWidget_(d);
        }
        clear_String(&d->sourceMime);
        d->sourceTime = response->when;
        updateTimestampBuf_DocumentWidget_(d);
        initBlock_String(&str, &response->body);
        if (isSuccess_GmStatusCode(statusCode)) {
            /* Check the MIME type. */
            iRangecc charset = range_CStr("utf-8");
            enum iGmDocumentFormat docFormat = undefined_GmDocumentFormat;
            const iString *mimeStr = collect_String(lower_String(&response->meta)); /* for convenience */
            set_String(&d->sourceMime, mimeStr);
            iRangecc mime = range_String(mimeStr);
            iRangecc seg = iNullRange;
            while (nextSplit_Rangecc(mime, ";", &seg)) {
                iRangecc param = seg;
                trim_Rangecc(&param);
                if (equal_Rangecc(param, "text/gemini")) {
                    docFormat = gemini_GmDocumentFormat;
                    setRange_String(&d->sourceMime, param);
                }
                else if (startsWith_Rangecc(param, "text/") ||
                         equal_Rangecc(param, "application/json")) {
                    docFormat = plainText_GmDocumentFormat;
                    setRange_String(&d->sourceMime, param);
                }
                else if (startsWith_Rangecc(param, "image/") ||
                         startsWith_Rangecc(param, "audio/")) {
                    const iBool isAudio = startsWith_Rangecc(param, "audio/");
                    /* Make a simple document with an image or audio player. */
                    docFormat = gemini_GmDocumentFormat;
                    setRange_String(&d->sourceMime, param);
                    if ((isAudio && isInitialUpdate) || (!isAudio && isRequestFinished)) {
                        const char *linkTitle =
                            startsWith_String(mimeStr, "image/") ? "Image" : "Audio";
                        iUrl parts;
                        init_Url(&parts, d->mod.url);
                        if (!isEmpty_Range(&parts.path)) {
                            linkTitle =
                                baseName_Path(collect_String(newRange_String(parts.path))).start;
                        }
                        format_String(&str, "=> %s %s\n", cstr_String(d->mod.url), linkTitle);
                        setData_Media(media_GmDocument(d->doc),
                                      1,
                                      mimeStr,
                                      &response->body,
                                      !isRequestFinished ? partialData_MediaFlag : 0);
                        redoLayout_GmDocument(d->doc);
                    }
                    else if (isAudio && !isInitialUpdate) {
                        /* Update the audio content. */
                        setData_Media(media_GmDocument(d->doc),
                                      1,
                                      mimeStr,
                                      &response->body,
                                      !isRequestFinished ? partialData_MediaFlag : 0);
                        refresh_Widget(d);
                        setSource = iFalse;
                    }
                    else {
                        clear_String(&str);
                    }
                }
                else if (startsWith_Rangecc(param, "charset=")) {
                    charset = (iRangecc){ param.start + 8, param.end };
                    /* Remove whitespace and quotes. */
                    trim_Rangecc(&charset);
                    if (*charset.start == '"' && *charset.end == '"') {
                        charset.start++;
                        charset.end--;
                    }
                }
            }
            if (docFormat == undefined_GmDocumentFormat) {
                showErrorPage_DocumentWidget_(d, unsupportedMimeType_GmStatusCode, &response->meta);
                deinit_String(&str);
                return;
            }
            setFormat_GmDocument(d->doc, docFormat);
            /* Convert the source to UTF-8 if needed. */
            if (!equalCase_Rangecc(charset, "utf-8")) {
                set_String(&str,
                           collect_String(decode_Block(&str.chars, cstr_Rangecc(charset))));
            }
        }
        if (setSource) {
            setSource_DocumentWidget_(d, &str);
        }
        deinit_String(&str);
    }
}

static void fetch_DocumentWidget_(iDocumentWidget *d) {
    /* Forget the previous request. */
    if (d->request) {
        iRelease(d->request);
        d->request = NULL;
    }
    postCommandf_App("document.request.started doc:%p url:%s", d, cstr_String(d->mod.url));
    clear_ObjectList(d->media);
    d->certFlags = 0;
    d->flags &= ~showLinkNumbers_DocumentWidgetFlag;
    d->state = fetching_RequestState;
    set_Atomic(&d->isRequestUpdated, iFalse);
    d->request = new_GmRequest(certs_App());
    setUrl_GmRequest(d->request, d->mod.url);
    iConnect(GmRequest, d->request, updated, d, requestUpdated_DocumentWidget_);
    iConnect(GmRequest, d->request, finished, d, requestFinished_DocumentWidget_);
    submit_GmRequest(d->request);
}

static void updateTrust_DocumentWidget_(iDocumentWidget *d, const iGmResponse *response) {
    if (response) {
        d->certFlags  = response->certFlags;
        d->certExpiry = response->certValidUntil;
        set_Block(d->certFingerprint, &response->certFingerprint);
        set_String(d->certSubject, &response->certSubject);
    }
    iLabelWidget *lock = findWidget_App("navbar.lock");
    if (~d->certFlags & available_GmCertFlag) {
        setFlags_Widget(as_Widget(lock), disabled_WidgetFlag, iTrue);
        updateTextCStr_LabelWidget(lock, gray50_ColorEscape openLock_Icon);
        return;
    }
    setFlags_Widget(as_Widget(lock), disabled_WidgetFlag, iFalse);
    const iBool isDarkMode = isDark_ColorTheme(colorTheme_App());
    if (~d->certFlags & domainVerified_GmCertFlag) {
        updateTextCStr_LabelWidget(lock, red_ColorEscape warning_Icon);
    }
    else if (d->certFlags & trusted_GmCertFlag) {
        updateTextCStr_LabelWidget(lock, green_ColorEscape closedLock_Icon);
    }
    else {
        updateTextCStr_LabelWidget(lock, isDarkMode ? orange_ColorEscape warning_Icon
            : black_ColorEscape warning_Icon);
    }
    setBanner_GmDocument(d->doc, bannerType_DocumentWidget_(d));
}

static void parseUser_DocumentWidget_(iDocumentWidget *d) {
    setRange_String(d->titleUser, urlUser_String(d->mod.url));
}

static iBool updateFromHistory_DocumentWidget_(iDocumentWidget *d) {
    const iRecentUrl *recent = findUrl_History(d->mod.history, d->mod.url);
    if (recent && recent->cachedResponse) {
        const iGmResponse *resp = recent->cachedResponse;
        clear_ObjectList(d->media);
        reset_GmDocument(d->doc);
        d->state = fetching_RequestState;
        d->initNormScrollY = recent->normScrollY;
        resetWideRuns_DocumentWidget_(d);
        /* Use the cached response data. */
        updateTrust_DocumentWidget_(d, resp);
        d->sourceTime = resp->when;
        d->sourceStatus = success_GmStatusCode;
        format_String(&d->sourceHeader, "(cached content)");
        updateTimestampBuf_DocumentWidget_(d);
        set_Block(&d->sourceContent, &resp->body);
        updateDocument_DocumentWidget_(d, resp, iTrue);
        init_Anim(&d->scrollY, d->initNormScrollY * size_GmDocument(d->doc).y);
        d->state = ready_RequestState;
        updateSideOpacity_DocumentWidget_(d, iFalse);
        updateSideIconBuf_DocumentWidget_(d);
        updateVisible_DocumentWidget_(d);
        postCommandf_App("document.changed doc:%p url:%s", d, cstr_String(d->mod.url));
        return iTrue;
    }
    else if (!isEmpty_String(d->mod.url)) {
        fetch_DocumentWidget_(d);
    }
    return iFalse;
}

static void refreshWhileScrolling_DocumentWidget_(iAny *ptr) {
    iDocumentWidget *d = ptr;
    updateVisible_DocumentWidget_(d);
    refresh_Widget(d);
    if (d->animWideRunId) {
        for (const iGmRun *r = d->animWideRunRange.start; r != d->animWideRunRange.end; r++) {
            insert_PtrSet(d->invalidRuns, r);
        }
    }
    if (isFinished_Anim(&d->animWideRunOffset)) {
        d->animWideRunId = 0;
    }
    if (!isFinished_Anim(&d->scrollY) || !isFinished_Anim(&d->animWideRunOffset)) {
        addTicker_App(refreshWhileScrolling_DocumentWidget_, d);
    }
}

static void smoothScroll_DocumentWidget_(iDocumentWidget *d, int offset, int duration) {
    /* Get rid of link numbers when scrolling. */
    if (offset && d->flags & showLinkNumbers_DocumentWidgetFlag) {
        d->flags &= ~showLinkNumbers_DocumentWidgetFlag;
        invalidateVisibleLinks_DocumentWidget_(d);
    }
    if (!prefs_App()->smoothScrolling) {
        duration = 0; /* always instant */
    }
    int destY = targetValue_Anim(&d->scrollY) + offset;
    if (destY < 0) {
        destY = 0;
    }
    const int scrollMax = scrollMax_DocumentWidget_(d);
    if (scrollMax > 0) {
        destY = iMin(destY, scrollMax);
    }
    else {
        destY = 0;
    }
    if (duration) {
        setValueEased_Anim(&d->scrollY, destY, duration);
    }
    else {
        setValue_Anim(&d->scrollY, destY, 0);
    }
    updateVisible_DocumentWidget_(d);
    refresh_Widget(as_Widget(d));
    if (duration > 0) {
        iChangeFlags(d->flags, noHoverWhileScrolling_DocumentWidgetFlag, iTrue);
        addTicker_App(refreshWhileScrolling_DocumentWidget_, d);
    }
}

static void scroll_DocumentWidget_(iDocumentWidget *d, int offset) {
    smoothScroll_DocumentWidget_(d, offset, 0 /* instantly */);
}

static void scrollTo_DocumentWidget_(iDocumentWidget *d, int documentY, iBool centered) {
    if (!hasSiteBanner_GmDocument(d->doc)) {
        documentY += d->pageMargin * gap_UI;
    }
    init_Anim(&d->scrollY,
              documentY - (centered ? documentBounds_DocumentWidget_(d).size.y / 2
                                    : lineHeight_Text(paragraph_FontId)));
    scroll_DocumentWidget_(d, 0); /* clamp it */
}

static void scrollToHeading_DocumentWidget_(iDocumentWidget *d, const char *heading) {
    iConstForEach(Array, h, headings_GmDocument(d->doc)) {
        const iGmHeading *head = h.value;
        if (startsWithCase_Rangecc(head->text, heading)) {
            postCommandf_App("document.goto loc:%p", head->text.start);
            break;
        }
    }
}

static void scrollWideBlock_DocumentWidget_(iDocumentWidget *d, iInt2 mousePos, int delta,
                                            int duration) {
    if (delta == 0) {
        return;
    }
    const iInt2 docPos = documentPos_DocumentWidget_(d, mousePos);
    iConstForEach(PtrArray, i, &d->visibleWideRuns) {
        const iGmRun *run = i.ptr;
        if (docPos.y >= top_Rect(run->bounds) && docPos.y <= bottom_Rect(run->bounds)) {
            /* We can scroll this run. First find out how much is allowed. */
            const iGmRunRange range = findPreformattedRange_GmDocument(d->doc, run);
            int maxWidth = 0;
            for (const iGmRun *r = range.start; r != range.end; r++) {
                maxWidth = iMax(maxWidth, width_Rect(r->visBounds));
            }
            const int maxOffset = maxWidth - documentWidth_DocumentWidget_(d) + d->pageMargin * gap_UI;
            if (size_Array(&d->wideRunOffsets) <= run->preId) {
                resize_Array(&d->wideRunOffsets, run->preId + 1);
            }
            int *offset = at_Array(&d->wideRunOffsets, run->preId - 1);
            const int oldOffset = *offset;
            *offset = iClamp(*offset + delta, 0, maxOffset);
            /* Make sure the whole block gets redraw. */
            if (oldOffset != *offset) {
                for (const iGmRun *r = range.start; r != range.end; r++) {
                    insert_PtrSet(d->invalidRuns, r);
                }
                refresh_Widget(d);
                d->selectMark = iNullRange;
                d->foundMark  = iNullRange;
            }
            if (duration) {
                if (d->animWideRunId != run->preId || isFinished_Anim(&d->animWideRunOffset)) {
                    d->animWideRunId = run->preId;
                    init_Anim(&d->animWideRunOffset, oldOffset);
                }
                setValueEased_Anim(&d->animWideRunOffset, *offset, duration);
                d->animWideRunRange = range;
                addTicker_App(refreshWhileScrolling_DocumentWidget_, d);
            }
            else {
                d->animWideRunId = 0;
                init_Anim(&d->animWideRunOffset, 0);
            }
            break;
        }
    }
}

static void checkResponse_DocumentWidget_(iDocumentWidget *d) {
    if (!d->request) {
        return;
    }
    enum iGmStatusCode statusCode = status_GmRequest(d->request);
    if (statusCode == none_GmStatusCode) {
        return;
    }
    iGmResponse *resp = lockResponse_GmRequest(d->request);
    if (d->state == fetching_RequestState) {
        d->state = receivedPartialResponse_RequestState;
        updateTrust_DocumentWidget_(d, resp);
        init_Anim(&d->sideOpacity, 0);
        format_String(&d->sourceHeader, "%d %s", statusCode, get_GmError(statusCode)->title);
        d->sourceStatus = statusCode;
        switch (category_GmStatusCode(statusCode)) {
            case categoryInput_GmStatusCode: {
                iUrl parts;
                init_Url(&parts, d->mod.url);
                iWidget *dlg = makeValueInput_Widget(
                    as_Widget(d),
                    NULL,
                    format_CStr(uiHeading_ColorEscape "%s", cstr_Rangecc(parts.host)),
                    isEmpty_String(&resp->meta)
                        ? format_CStr("Please enter input for %s:", cstr_Rangecc(parts.path))
                        : cstr_String(&resp->meta),
                    uiTextCaution_ColorEscape "Send \u21d2",
                    format_CStr("!document.input.submit doc:%p", d));
                setSensitiveContent_InputWidget(findChild_Widget(dlg, "input"),
                                                statusCode == sensitiveInput_GmStatusCode);
                if (document_App() != d) {
                    postCommandf_App("tabs.switch page:%p", d);
                }
                break;
            }
            case categorySuccess_GmStatusCode:
                init_Anim(&d->scrollY, 0);
                reset_GmDocument(d->doc); /* new content incoming */
                resetWideRuns_DocumentWidget_(d);
                updateDocument_DocumentWidget_(d, resp, iTrue);
                break;
            case categoryRedirect_GmStatusCode:
                if (isEmpty_String(&resp->meta)) {
                    showErrorPage_DocumentWidget_(d, invalidRedirect_GmStatusCode, NULL);
                }
                else {
                    /* Only accept redirects that use gemini scheme. */
                    const iString *dstUrl = absoluteUrl_String(d->mod.url, &resp->meta);
                    if (d->redirectCount >= 5) {
                        showErrorPage_DocumentWidget_(d, tooManyRedirects_GmStatusCode, dstUrl);
                    }
                    else if (equalCase_Rangecc(urlScheme_String(dstUrl),
                                               cstr_Rangecc(urlScheme_String(d->mod.url)))) {
                        /* Redirects with the same scheme are automatic. */
                        visitUrl_Visited(visited_App(), d->mod.url, transient_VisitedUrlFlag);
                        postCommandf_App(
                            "open doc:%p redirect:%d url:%s", d, d->redirectCount + 1, cstr_String(dstUrl));
                    }
                    else {
                        /* Scheme changes must be manually approved. */
                        showErrorPage_DocumentWidget_(d, schemeChangeRedirect_GmStatusCode, dstUrl);
                    }
                    unlockResponse_GmRequest(d->request);
                    iReleasePtr(&d->request);
                }
                break;
            default:
                if (isDefined_GmError(statusCode)) {
                    showErrorPage_DocumentWidget_(d, statusCode, &resp->meta);
                }
                else if (category_GmStatusCode(statusCode) ==
                         categoryTemporaryFailure_GmStatusCode) {
                    showErrorPage_DocumentWidget_(
                        d, temporaryFailure_GmStatusCode, &resp->meta);
                }
                else if (category_GmStatusCode(statusCode) ==
                         categoryPermanentFailure_GmStatusCode) {
                    showErrorPage_DocumentWidget_(
                        d, permanentFailure_GmStatusCode, &resp->meta);
                }
                else {
                    showErrorPage_DocumentWidget_(d, unknownStatusCode_GmStatusCode, &resp->meta);
                }
                break;
        }
    }
    else if (d->state == receivedPartialResponse_RequestState) {
        switch (category_GmStatusCode(statusCode)) {
            case categorySuccess_GmStatusCode:
                /* More content available. */
                updateDocument_DocumentWidget_(d, resp, iFalse);
                break;
            default:
                break;
        }
    }
    unlockResponse_GmRequest(d->request);
}

static const char *sourceLoc_DocumentWidget_(const iDocumentWidget *d, iInt2 pos) {
    return findLoc_GmDocument(d->doc, documentPos_DocumentWidget_(d, pos));
}

iDeclareType(MiddleRunParams)

struct Impl_MiddleRunParams {
    int midY;
    const iGmRun *closest;
    int distance;
};

static void find_MiddleRunParams_(void *params, const iGmRun *run) {
    iMiddleRunParams *d = params;
    if (isEmpty_Rect(run->bounds)) {
        return;
    }
    const int distance = iAbs(mid_Rect(run->bounds).y - d->midY);
    if (!d->closest || distance < d->distance) {
        d->closest  = run;
        d->distance = distance;
    }
}

static const iGmRun *middleRun_DocumentWidget_(const iDocumentWidget *d) {
    iRangei visRange = visibleRange_DocumentWidget_(d);
    iMiddleRunParams params = { (visRange.start + visRange.end) / 2, NULL, 0 };
    render_GmDocument(d->doc, visRange, find_MiddleRunParams_, &params);
    return params.closest;
}

static void removeMediaRequest_DocumentWidget_(iDocumentWidget *d, iGmLinkId linkId) {
    iForEach(ObjectList, i, d->media) {
        iMediaRequest *req = (iMediaRequest *) i.object;
        if (req->linkId == linkId) {
            remove_ObjectListIterator(&i);
            break;
        }
    }
}

static iMediaRequest *findMediaRequest_DocumentWidget_(const iDocumentWidget *d, iGmLinkId linkId) {
    iConstForEach(ObjectList, i, d->media) {
        const iMediaRequest *req = (const iMediaRequest *) i.object;
        if (req->linkId == linkId) {
            return iConstCast(iMediaRequest *, req);
        }
    }
    return NULL;
}

static iBool requestMedia_DocumentWidget_(iDocumentWidget *d, iGmLinkId linkId, iBool enableFilters) {
    if (!findMediaRequest_DocumentWidget_(d, linkId)) {
        const iString *mediaUrl = absoluteUrl_String(d->mod.url, linkUrl_GmDocument(d->doc, linkId));
        pushBack_ObjectList(d->media, iClob(new_MediaRequest(d, linkId, mediaUrl, enableFilters)));
        invalidate_DocumentWidget_(d);
        return iTrue;
    }
    return iFalse;
}

static iBool isDownloadRequest_DocumentWidget(const iDocumentWidget *d, const iMediaRequest *req) {
    return findLinkDownload_Media(constMedia_GmDocument(d->doc), req->linkId) != 0;
}

static iBool handleMediaCommand_DocumentWidget_(iDocumentWidget *d, const char *cmd) {
    iMediaRequest *req = pointerLabel_Command(cmd, "request");
    iBool isOurRequest = iFalse;
    /* This request may already be deleted so treat the pointer with caution. */
    iConstForEach(ObjectList, m, d->media) {
        if (m.object == req) {
            isOurRequest = iTrue;
            break;
        }
    }
    if (!isOurRequest) {
        return iFalse;
    }
    if (equal_Command(cmd, "media.updated")) {
        /* Pass new data to media players. */
        const enum iGmStatusCode code = status_GmRequest(req->req);
        if (isSuccess_GmStatusCode(code)) {
            iGmResponse *resp = lockResponse_GmRequest(req->req);
            if (isDownloadRequest_DocumentWidget(d, req) ||
                startsWith_String(&resp->meta, "audio/")) {
                /* TODO: Use a helper? This is same as below except for the partialData flag. */
                if (setData_Media(media_GmDocument(d->doc),
                                  req->linkId,
                                  &resp->meta,
                                  &resp->body,
                                  partialData_MediaFlag | allowHide_MediaFlag)) {
                    redoLayout_GmDocument(d->doc);
                }
                updateVisible_DocumentWidget_(d);
                invalidate_DocumentWidget_(d);
                refresh_Widget(as_Widget(d));
            }
            unlockResponse_GmRequest(req->req);
        }
        /* Update the link's progress. */
        invalidateLink_DocumentWidget_(d, req->linkId);
        refresh_Widget(d);
        return iTrue;
    }
    else if (equal_Command(cmd, "media.finished")) {
        const enum iGmStatusCode code = status_GmRequest(req->req);
        /* Give the media to the document for presentation. */
        if (isSuccess_GmStatusCode(code)) {
            if (isDownloadRequest_DocumentWidget(d, req) ||
                startsWith_String(meta_GmRequest(req->req), "image/") ||
                startsWith_String(meta_GmRequest(req->req), "audio/")) {
                setData_Media(media_GmDocument(d->doc),
                              req->linkId,
                              meta_GmRequest(req->req),
                              body_GmRequest(req->req),
                              allowHide_MediaFlag);
                redoLayout_GmDocument(d->doc);
                updateVisible_DocumentWidget_(d);
                invalidate_DocumentWidget_(d);
                refresh_Widget(as_Widget(d));
            }
        }
        else {
            const iGmError *err = get_GmError(code);
            makeMessage_Widget(format_CStr(uiTextCaution_ColorEscape "%s", err->title), err->info);
            removeMediaRequest_DocumentWidget_(d, req->linkId);
        }
        return iTrue;
    }
    return iFalse;
}

static void allocVisBuffer_DocumentWidget_(const iDocumentWidget *d) {
    const iWidget *w         = constAs_Widget(d);
    const iBool    isVisible = isVisible_Widget(w);
    const iInt2    size      = bounds_Widget(w).size;
    if (isVisible) {
        alloc_VisBuf(d->visBuf, size, 1);
    }
    else {
        dealloc_VisBuf(d->visBuf);
    }
}

static iBool fetchNextUnfetchedImage_DocumentWidget_(iDocumentWidget *d) {
    iConstForEach(PtrArray, i, &d->visibleLinks) {
        const iGmRun *run = i.ptr;
        if (run->linkId && run->mediaType == none_GmRunMediaType &&
            ~run->flags & decoration_GmRunFlag) {
            const int linkFlags = linkFlags_GmDocument(d->doc, run->linkId);
            if (isMediaLink_GmDocument(d->doc, run->linkId) &&
                linkFlags & imageFileExtension_GmLinkFlag &&
                ~linkFlags & content_GmLinkFlag && ~linkFlags & permanent_GmLinkFlag ) {
                if (requestMedia_DocumentWidget_(d, run->linkId, iTrue)) {
                    return iTrue;
                }
            }
        }
    }
    return iFalse;
}

static void saveToDownloads_(const iString *url, const iString *mime, const iBlock *content) {
    const iString *savePath = downloadPathForUrl_App(url, mime);
    /* Write the file. */ {
        iFile *f = new_File(savePath);
        if (open_File(f, writeOnly_FileMode)) {
            write_File(f, content);
            const size_t size   = size_Block(content);
            const iBool  isMega = size >= 1000000;
            makeMessage_Widget(uiHeading_ColorEscape "FILE SAVED",
                               format_CStr("%s\nSize: %.3f %s", cstr_String(path_File(f)),
                                           isMega ? size / 1.0e6f : (size / 1.0e3f),
                                           isMega ? "MB" : "KB"));
        }
        else {
            makeMessage_Widget(uiTextCaution_ColorEscape "ERROR SAVING FILE",
                               strerror(errno));
        }
        iRelease(f);
    }
}

static void addAllLinks_(void *context, const iGmRun *run) {
    iPtrArray *links = context;
    if (~run->flags & decoration_GmRunFlag && run->linkId) {
        pushBack_PtrArray(links, run);
    }
}

static size_t visibleLinkOrdinal_DocumentWidget_(const iDocumentWidget *d, iGmLinkId linkId) {
    size_t ord = 0;
    const iRangei visRange = visibleRange_DocumentWidget_(d);
    iConstForEach(PtrArray, i, &d->visibleLinks) {
        const iGmRun *run = i.ptr;
        if (top_Rect(run->visBounds) >= visRange.start + gap_UI * d->pageMargin * 4 / 5) {
            if (run->flags & decoration_GmRunFlag && run->linkId) {
                if (run->linkId == linkId) return ord;
                ord++;
            }
        }
    }
    return iInvalidPos;
}

/* Sorted by proximity to F and J. */
static const int homeRowKeys_[] = {
    'f', 'd', 's', 'a',
    'j', 'k', 'l',
    'r', 'e', 'w', 'q',
    'u', 'i', 'o', 'p',
    'v', 'c', 'x', 'z',
    'm', 'n',
    'g', 'h',
    'b',
    't', 'y',
};

static void updateDocumentWidthRetainingScrollPosition_DocumentWidget_(iDocumentWidget *d,
                                                                       iBool keepCenter) {
    /* Font changes (i.e., zooming) will keep the view centered, otherwise keep the top
       of the visible area fixed. */
    const iGmRun *run     = keepCenter ? middleRun_DocumentWidget_(d) : d->firstVisibleRun;
    const char *  runLoc  = (run ? run->text.start : NULL);
    int           voffset = 0;
    if (!keepCenter && run) {
        /* Keep the first visible run visible at the same position. */
        /* TODO: First *fully* visible run? */
        voffset = visibleRange_DocumentWidget_(d).start - top_Rect(run->visBounds);
    }
    setWidth_GmDocument(d->doc, documentWidth_DocumentWidget_(d));
    documentRunsInvalidated_DocumentWidget_(d);
    if (runLoc && !keepCenter) {
        run = findRunAtLoc_GmDocument(d->doc, runLoc);
        if (run) {
            scrollTo_DocumentWidget_(d,
                                     top_Rect(run->visBounds) +
                                         lineHeight_Text(paragraph_FontId) + voffset,
                                     iFalse);
        }
    }
    else if (runLoc && keepCenter) {
        run = findRunAtLoc_GmDocument(d->doc, runLoc);
        if (run) {
            scrollTo_DocumentWidget_(d, mid_Rect(run->bounds).y, iTrue);
        }
    }
}

static iBool scroll_page(iDocumentWidget *d, const char *cmd, float amt) {
    const int dir = arg_Command(cmd);
    if (dir > 0 && !argLabel_Command(cmd, "repeat") &&
        prefs_App()->loadImageInsteadOfScrolling &&
        fetchNextUnfetchedImage_DocumentWidget_(d)) {
        return iTrue;
    }
    smoothScroll_DocumentWidget_(d,
                                 dir * (amt * height_Rect(documentBounds_DocumentWidget_(d)) -
                                        0 * lineHeight_Text(paragraph_FontId)),
                                 smoothDuration_DocumentWidget_);
    return iTrue;
}

static iBool handleCommand_DocumentWidget_(iDocumentWidget *d, const char *cmd) {
    iWidget *w = as_Widget(d);
    if (equal_Command(cmd, "window.resized") || equal_Command(cmd, "font.changed")) {
        /* Alt/Option key may be involved in window size changes. */
        iChangeFlags(d->flags, showLinkNumbers_DocumentWidgetFlag, iFalse);
        const iBool keepCenter = equal_Command(cmd, "font.changed");
        updateDocumentWidthRetainingScrollPosition_DocumentWidget_(d, keepCenter);
        updateSideIconBuf_DocumentWidget_(d);
        invalidate_DocumentWidget_(d);
        dealloc_VisBuf(d->visBuf);
        updateWindowTitle_DocumentWidget_(d);
        refresh_Widget(w);
    }
    else if (equal_Command(cmd, "window.focus.lost")) {
        if (d->flags & showLinkNumbers_DocumentWidgetFlag) {
            d->flags &= ~showLinkNumbers_DocumentWidgetFlag;
            invalidateVisibleLinks_DocumentWidget_(d);
            refresh_Widget(w);
        }
        return iFalse;
    }
    else if (equal_Command(cmd, "window.mouse.exited")) {
        return iFalse;
    }
    else if (equal_Command(cmd, "theme.changed") && document_App() == d) {
        updateTheme_DocumentWidget_(d);
        updateVisible_DocumentWidget_(d);
        updateTrust_DocumentWidget_(d, NULL);
        updateSideIconBuf_DocumentWidget_(d);
        invalidate_DocumentWidget_(d);
        refresh_Widget(w);
    }
    else if (equal_Command(cmd, "document.layout.changed") && document_App() == d) {
        updateSize_DocumentWidget(d);
    }
    else if (equal_Command(cmd, "tabs.changed")) {
        iChangeFlags(d->flags, showLinkNumbers_DocumentWidgetFlag, iFalse);
        if (cmp_String(id_Widget(w), suffixPtr_Command(cmd, "id")) == 0) {
            /* Set palette for our document. */
            updateTheme_DocumentWidget_(d);
            updateTrust_DocumentWidget_(d, NULL);
            updateSize_DocumentWidget(d);
            updateFetchProgress_DocumentWidget_(d);
        }
        init_Anim(&d->sideOpacity, 0);
        updateSideOpacity_DocumentWidget_(d, iFalse);
        updateWindowTitle_DocumentWidget_(d);
        allocVisBuffer_DocumentWidget_(d);
        animateMedia_DocumentWidget_(d);
        return iFalse;
    }
    else if (equal_Command(cmd, "tab.created")) {
        /* Space for tab buttons has changed. */
        updateWindowTitle_DocumentWidget_(d);
        return iFalse;
    }
    else if (equal_Command(cmd, "document.info") && d == document_App()) {
        const char *unchecked       = red_ColorEscape "\u2610";
        const char *checked         = green_ColorEscape "\u2611";
        const iBool haveFingerprint = (d->certFlags & haveFingerprint_GmCertFlag) != 0;
        const iBool canTrust =
            (d->certFlags == (available_GmCertFlag | haveFingerprint_GmCertFlag |
                              timeVerified_GmCertFlag | domainVerified_GmCertFlag));
        const iRecentUrl *recent = findUrl_History(d->mod.history, d->mod.url);
        const iString *meta = &d->sourceMime;
        if (recent && recent->cachedResponse) {
            meta = &recent->cachedResponse->meta;
        }
        iString *msg = collectNew_String();
        if (isEmpty_String(&d->sourceHeader)) {
            appendFormat_String(msg, "%s\n%zu bytes\n", cstr_String(meta), size_Block(&d->sourceContent));
        }
        else {
            appendFormat_String(msg, "%s\n", cstr_String(&d->sourceHeader));
            if (size_Block(&d->sourceContent)) {
                appendFormat_String(msg, "%zu bytes\n", size_Block(&d->sourceContent));
            }
        }
        appendFormat_String(msg,
                      "\n%sCertificate Status:\n"
                      "%s%s  %s by CA\n"
                      "%s%s  Domain name %s%s\n"
                      "%s%s  %s (%04d-%02d-%02d %02d:%02d:%02d)\n"
                      "%s%s  %s",
                      uiHeading_ColorEscape,
                      d->certFlags & authorityVerified_GmCertFlag ?
                            checked : uiTextAction_ColorEscape "\u2610",
                      uiText_ColorEscape,
                      d->certFlags & authorityVerified_GmCertFlag ? "Verified" : "Not verified",
                      d->certFlags & domainVerified_GmCertFlag ? checked : unchecked,
                      uiText_ColorEscape,
                      d->certFlags & domainVerified_GmCertFlag ? "matches" : "mismatch",
                      ~d->certFlags & domainVerified_GmCertFlag
                          ? format_CStr(" (%s)", cstr_String(d->certSubject))
                          : "",
                      d->certFlags & timeVerified_GmCertFlag ? checked : unchecked,
                      uiText_ColorEscape,
                      d->certFlags & timeVerified_GmCertFlag ? "Not expired" : "Expired",
                      d->certExpiry.year,
                      d->certExpiry.month,
                      d->certExpiry.day,
                      d->certExpiry.hour,
                      d->certExpiry.minute,
                      d->certExpiry.second,
                      d->certFlags & trusted_GmCertFlag ? checked : unchecked,
                      uiText_ColorEscape,
                      d->certFlags & trusted_GmCertFlag ? "Trusted" : "Not trusted");
        setFocus_Widget(NULL);
        iArray *items = new_Array(sizeof(iMenuItem));
        if (canTrust) {
            pushBack_Array(
                items, &(iMenuItem){ uiTextCaution_ColorEscape "Trust", 0, 0, "server.trustcert" });
        }
        if (haveFingerprint) {
            pushBack_Array(items, &(iMenuItem){ "Copy Fingerprint", 0, 0, "server.copycert" });
        }
        if (!isEmpty_Array(items)) {
            pushBack_Array(items, &(iMenuItem){ "---", 0, 0, 0 });
        }
        pushBack_Array(items, &(iMenuItem){ "Dismiss", 0, 0, "message.ok" });
        iWidget *dlg = makeQuestion_Widget(uiHeading_ColorEscape "PAGE INFORMATION",
                                           cstr_String(msg),
                                           data_Array(items),
                                           size_Array(items));
        delete_Array(items);
        /* Enforce a minimum size. */
        iWidget *sizer = new_Widget();
        setSize_Widget(sizer, init_I2(gap_UI * 90, 1));
        addChildFlags_Widget(dlg, iClob(sizer), frameless_WidgetFlag);
        setFlags_Widget(dlg, centerHorizontal_WidgetFlag, iFalse);
        setPos_Widget(dlg, bottomLeft_Rect(bounds_Widget(findWidget_App("navbar.lock"))));
        arrange_Widget(dlg);
        addAction_Widget(dlg, SDLK_ESCAPE, 0, "message.ok");
        addAction_Widget(dlg, SDLK_SPACE, 0, "message.ok");
        return iTrue;
    }
    else if (equal_Command(cmd, "server.trustcert") && document_App() == d) {
        const iRangecc host = urlHost_String(d->mod.url);
        if (!isEmpty_Block(d->certFingerprint) && !isEmpty_Range(&host)) {
            setTrusted_GmCerts(certs_App(), host, d->certFingerprint, &d->certExpiry);
            d->certFlags |= trusted_GmCertFlag;
            postCommand_App("document.info");
            updateTrust_DocumentWidget_(d, NULL);
            redoLayout_GmDocument(d->doc);
            invalidate_DocumentWidget_(d);
            refresh_Widget(d);
        }
        return iTrue;
    }
    else if (equal_Command(cmd, "server.copycert") && document_App() == d) {
        SDL_SetClipboardText(cstrCollect_String(hexEncode_Block(d->certFingerprint)));
        return iTrue;
    }
    else if (equal_Command(cmd, "copy") && document_App() == d && !focus_Widget()) {
        iString *copied;
        if (d->selectMark.start) {
            iRangecc mark = d->selectMark;
            if (mark.start > mark.end) {
                iSwap(const char *, mark.start, mark.end);
            }
            copied = newRange_String(mark);
        }
        else {
            /* Full document. */
            copied = copy_String(source_GmDocument(d->doc));
        }
        SDL_SetClipboardText(cstr_String(copied));
        delete_String(copied);
        return iTrue;
    }
    else if (equal_Command(cmd, "document.copylink") && document_App() == d) {
        if (d->contextLink) {
            SDL_SetClipboardText(cstr_String(withSpacesEncoded_String(absoluteUrl_String(
                d->mod.url, linkUrl_GmDocument(d->doc, d->contextLink->linkId)))));
        }
        else {
            SDL_SetClipboardText(cstr_String(withSpacesEncoded_String(d->mod.url)));
        }
        return iTrue;
    }
    else if (equalWidget_Command(cmd, w, "document.downloadlink")) {
        if (d->contextLink) {
            const iGmLinkId linkId = d->contextLink->linkId;
            setDownloadUrl_Media(
                media_GmDocument(d->doc), linkId, linkUrl_GmDocument(d->doc, linkId));
            requestMedia_DocumentWidget_(d, linkId, iFalse /* no filters */);
            redoLayout_GmDocument(d->doc); /* inline downloader becomes visible */
            updateVisible_DocumentWidget_(d);
            invalidate_DocumentWidget_(d);
            refresh_Widget(w);
        }
        return iTrue;
    }
    else if (equal_Command(cmd, "document.input.submit") && document_Command(cmd) == d) {
        iString *value = suffix_Command(cmd, "value");
        set_String(value, collect_String(urlEncode_String(value)));
        iString *url = collect_String(copy_String(d->mod.url));
        const size_t qPos = indexOfCStr_String(url, "?");
        if (qPos != iInvalidPos) {
            remove_Block(&url->chars, qPos, iInvalidSize);
        }
        appendCStr_String(url, "?");
        append_String(url, value);
        postCommandf_App("open url:%s", cstr_String(url));
        delete_String(value);
        return iTrue;
    }
    else if (equal_Command(cmd, "valueinput.cancelled") &&
             equal_Rangecc(range_Command(cmd, "id"), "document.input.submit") && document_App() == d) {
        postCommand_App("navigate.back");
        return iTrue;
    }
    else if (equalWidget_Command(cmd, w, "document.request.updated") &&
             d->request && pointerLabel_Command(cmd, "request") == d->request) {
        set_Block(&d->sourceContent, &lockResponse_GmRequest(d->request)->body);
        unlockResponse_GmRequest(d->request);
        if (document_App() == d) {
            updateFetchProgress_DocumentWidget_(d);
        }
        checkResponse_DocumentWidget_(d);
        set_Atomic(&d->isRequestUpdated, iFalse); /* ready to be notified again */
        return iFalse;
    }
    else if (equalWidget_Command(cmd, w, "document.request.finished") &&
             pointerLabel_Command(cmd, "request") == d->request) {
        set_Block(&d->sourceContent, body_GmRequest(d->request));
        if (!isSuccess_GmStatusCode(status_GmRequest(d->request))) {
            format_String(&d->sourceHeader,
                          "%d %s",
                          status_GmRequest(d->request),
                          cstr_String(meta_GmRequest(d->request)));
        }
        else {
            clear_String(&d->sourceHeader);
        }
        updateFetchProgress_DocumentWidget_(d);
        checkResponse_DocumentWidget_(d);
        init_Anim(&d->scrollY, d->initNormScrollY * size_GmDocument(d->doc).y);
        d->state = ready_RequestState;
        /* The response may be cached. */ {
            if (!equal_Rangecc(urlScheme_String(d->mod.url), "about") &&
                startsWithCase_String(meta_GmRequest(d->request), "text/")) {
                setCachedResponse_History(d->mod.history, lockResponse_GmRequest(d->request));
                unlockResponse_GmRequest(d->request);
            }
        }
        iReleasePtr(&d->request);
        updateVisible_DocumentWidget_(d);
        updateSideIconBuf_DocumentWidget_(d);
        postCommandf_App("document.changed doc:%p url:%s", d, cstr_String(d->mod.url));
        /* Check for a pending goto. */
        if (!isEmpty_String(&d->pendingGotoHeading)) {
            scrollToHeading_DocumentWidget_(d, cstr_String(&d->pendingGotoHeading));
            clear_String(&d->pendingGotoHeading);
        }
        return iFalse;
    }
    else if (equal_Command(cmd, "media.updated") || equal_Command(cmd, "media.finished")) {
        return handleMediaCommand_DocumentWidget_(d, cmd);
    }
    else if (equal_Command(cmd, "media.player.started")) {
        /* When one media player starts, pause the others that may be playing. */
        const iPlayer *startedPlr = pointerLabel_Command(cmd, "player");
        const iMedia * media  = media_GmDocument(d->doc);
        const size_t   num    = numAudio_Media(media);
        for (size_t id = 1; id <= num; id++) {
            iPlayer *plr = audioPlayer_Media(media, id);
            if (plr != startedPlr) {
                setPaused_Player(plr, iTrue);
            }
        }
    }
    else if (equal_Command(cmd, "media.player.update")) {
        updateMedia_DocumentWidget_(d);
        return iFalse;
    }
    else if (equal_Command(cmd, "document.stop") && document_App() == d) {
        if (d->request) {
            postCommandf_App(
                "document.request.cancelled doc:%p url:%s", d, cstr_String(d->mod.url));
            iReleasePtr(&d->request);
            if (d->state != ready_RequestState) {
                d->state = ready_RequestState;
                postCommand_App("navigate.back");
            }
            updateFetchProgress_DocumentWidget_(d);
            return iTrue;
        }
    }
    else if (equalWidget_Command(cmd, w, "document.media.save")) {
        const iGmLinkId      linkId = argLabel_Command(cmd, "link");
        const iMediaRequest *media  = findMediaRequest_DocumentWidget_(d, linkId);
        if (media) {
            saveToDownloads_(url_GmRequest(media->req), meta_GmRequest(media->req),
                             body_GmRequest(media->req));
        }
    }
    else if (equal_Command(cmd, "document.save") && document_App() == d) {
        if (d->request) {
            makeMessage_Widget(uiTextCaution_ColorEscape "PAGE INCOMPLETE",
                               "The page contents are still being downloaded.");
        }
        else if (!isEmpty_Block(&d->sourceContent)) {
            saveToDownloads_(d->mod.url, &d->sourceMime, &d->sourceContent);
        }
        return iTrue;
    }
    else if (equal_Command(cmd, "document.reload") && document_Command(cmd) == d) {
        d->initNormScrollY = normScrollPos_DocumentWidget_(d);
        fetch_DocumentWidget_(d);
        return iTrue;
    }
    else if (equal_Command(cmd, "document.linkkeys") && document_App() == d) {
        if (argLabel_Command(cmd, "release")) {
            iChangeFlags(d->flags, showLinkNumbers_DocumentWidgetFlag, iFalse);
        }
        else if (argLabel_Command(cmd, "more")) {
            if (d->flags & showLinkNumbers_DocumentWidgetFlag &&
                d->ordinalMode == homeRow_DocumentLinkOrdinalMode) {
                const size_t numKeys = iElemCount(homeRowKeys_);
                const iGmRun *last = lastVisibleLink_DocumentWidget_(d);
                if (!last) {
                    d->ordinalBase = 0;
                }
                else {
                    d->ordinalBase += numKeys;
                    if (visibleLinkOrdinal_DocumentWidget_(d, last->linkId) < d->ordinalBase) {
                        d->ordinalBase = 0;
                    }
                }
            }
            else if (~d->flags & showLinkNumbers_DocumentWidgetFlag) {
                d->ordinalMode = homeRow_DocumentLinkOrdinalMode;
                d->ordinalBase = 0;
                iChangeFlags(d->flags, showLinkNumbers_DocumentWidgetFlag, iTrue);
            }
        }
        else {
            d->ordinalMode = arg_Command(cmd);
            d->ordinalBase = 0;
            iChangeFlags(d->flags, showLinkNumbers_DocumentWidgetFlag, iTrue);
            iChangeFlags(d->flags, setHoverViaKeys_DocumentWidgetFlag,
                         argLabel_Command(cmd, "hover") != 0);
            iChangeFlags(d->flags, newTabViaHomeKeys_DocumentWidgetFlag,
                         argLabel_Command(cmd, "newtab") != 0);
        }
        invalidateVisibleLinks_DocumentWidget_(d);
        refresh_Widget(d);
        return iTrue;
    }
    else if (equal_Command(cmd, "navigate.back") && document_App() == d) {
        if (d->request) {
            postCommandf_App(
                "document.request.cancelled doc:%p url:%s", d, cstr_String(d->mod.url));
            iReleasePtr(&d->request);
            updateFetchProgress_DocumentWidget_(d);
        }
        goBack_History(d->mod.history);
        return iTrue;
    }
    else if (equal_Command(cmd, "navigate.forward") && document_App() == d) {
        goForward_History(d->mod.history);
        return iTrue;
    }
    else if (equal_Command(cmd, "navigate.parent") && document_App() == d) {
        iUrl parts;
        init_Url(&parts, d->mod.url);
        /* Remove the last path segment. */
        if (size_Range(&parts.path) > 1) {
            if (parts.path.end[-1] == '/') {
                parts.path.end--;
            }
            while (parts.path.end > parts.path.start) {
                if (parts.path.end[-1] == '/') break;
                parts.path.end--;
            }
            postCommandf_App(
                "open url:%s",
                cstr_Rangecc((iRangecc){ constBegin_String(d->mod.url), parts.path.end }));
        }
        return iTrue;
    }
    else if (equal_Command(cmd, "navigate.root") && document_App() == d) {
        postCommandf_App("open url:%s/", cstr_Rangecc(urlRoot_String(d->mod.url)));
        return iTrue;
    }
    else if (equalWidget_Command(cmd, w, "scroll.moved")) {
        init_Anim(&d->scrollY, arg_Command(cmd));
        updateVisible_DocumentWidget_(d);
        return iTrue;
    }
    else if (equal_Command(cmd, "scroll.page") && document_App() == d) {
        return scroll_page(d, cmd, 0.5f);
    }
    else if (equal_Command(cmd, "scroll.fullpage") && document_App() == d) {
        return scroll_page(d, cmd, 1.0f);
    }
    else if (equal_Command(cmd, "scroll.top") && document_App() == d) {
        init_Anim(&d->scrollY, 0);
        invalidate_VisBuf(d->visBuf);
        scroll_DocumentWidget_(d, 0);
        updateVisible_DocumentWidget_(d);
        refresh_Widget(w);
        return iTrue;
    }
    else if (equal_Command(cmd, "scroll.bottom") && document_App() == d) {
        init_Anim(&d->scrollY, scrollMax_DocumentWidget_(d));
        invalidate_VisBuf(d->visBuf);
        scroll_DocumentWidget_(d, 0);
        updateVisible_DocumentWidget_(d);
        refresh_Widget(w);
        return iTrue;
    }
    else if (equal_Command(cmd, "scroll.step") && document_App() == d) {
        const int dir = arg_Command(cmd);
        if (dir > 0 && !argLabel_Command(cmd, "repeat") &&
            prefs_App()->loadImageInsteadOfScrolling &&
            fetchNextUnfetchedImage_DocumentWidget_(d)) {
            return iTrue;
        }
        smoothScroll_DocumentWidget_(d,
                                     3 * lineHeight_Text(paragraph_FontId) * dir,
                                     smoothDuration_DocumentWidget_);
        return iTrue;
    }
    else if (equal_Command(cmd, "document.goto") && document_App() == d) {
        const char *heading = suffixPtr_Command(cmd, "heading");
        if (heading) {
            if (isRequestOngoing_DocumentWidget(d)) {
                /* Scroll position set when request finishes. */
                setCStr_String(&d->pendingGotoHeading, heading);
                return iTrue;
            }
            scrollToHeading_DocumentWidget_(d, heading);
            return iTrue;
        }
        const char *loc = pointerLabel_Command(cmd, "loc");
        const iGmRun *run = findRunAtLoc_GmDocument(d->doc, loc);
        if (run) {
            scrollTo_DocumentWidget_(d, run->visBounds.pos.y, iFalse);
        }
        return iTrue;
    }
    else if ((equal_Command(cmd, "find.next") || equal_Command(cmd, "find.prev")) &&
             document_App() == d) {
        const int dir = equal_Command(cmd, "find.next") ? +1 : -1;
        iRangecc (*finder)(const iGmDocument *, const iString *, const char *) =
            dir > 0 ? findText_GmDocument : findTextBefore_GmDocument;
        iInputWidget *find = findWidget_App("find.input");
        if (isEmpty_String(text_InputWidget(find))) {
            d->foundMark = iNullRange;
        }
        else {
            const iBool wrap = d->foundMark.start != NULL;
            d->foundMark     = finder(d->doc, text_InputWidget(find), dir > 0 ? d->foundMark.end
                                                                          : d->foundMark.start);
            if (!d->foundMark.start && wrap) {
                /* Wrap around. */
                d->foundMark = finder(d->doc, text_InputWidget(find), NULL);
            }
            if (d->foundMark.start) {
                const iGmRun *found;
                if ((found = findRunAtLoc_GmDocument(d->doc, d->foundMark.start)) != NULL) {
                    scrollTo_DocumentWidget_(d, mid_Rect(found->bounds).y, iTrue);
                }
            }
        }
        invalidateWideRunsWithNonzeroOffset_DocumentWidget_(d); /* markers don't support offsets */
        resetWideRuns_DocumentWidget_(d);
        refresh_Widget(w);
        return iTrue;
    }
    else if (equal_Command(cmd, "find.clearmark")) {
        if (d->foundMark.start) {
            d->foundMark = iNullRange;
            refresh_Widget(w);
        }
        return iTrue;
    }
    else if (equal_Command(cmd, "bookmark.links") && document_App() == d) {
        iPtrArray *links = collectNew_PtrArray();
        render_GmDocument(d->doc, (iRangei){ 0, size_GmDocument(d->doc).y }, addAllLinks_, links);
        /* Find links that aren't already bookmarked. */
        iForEach(PtrArray, i, links) {
            const iGmRun *run = i.ptr;
            uint32_t      bmid;
            if ((bmid = findUrl_Bookmarks(bookmarks_App(),
                                          linkUrl_GmDocument(d->doc, run->linkId))) != 0) {
                const iBookmark *bm = get_Bookmarks(bookmarks_App(), bmid);
                /* We can import local copies of remote bookmarks. */
                if (!hasTag_Bookmark(bm, "remote")) {
                    remove_PtrArrayIterator(&i);
                }
            }
        }
        if (!isEmpty_PtrArray(links)) {
            if (argLabel_Command(cmd, "confirm")) {
                const char *plural = size_PtrArray(links) != 1 ? "s" : "";
                makeQuestion_Widget(
                    uiHeading_ColorEscape "IMPORT BOOKMARKS",
                    format_CStr("Found %d new link%s on the page.", size_PtrArray(links), plural),
                    (iMenuItem[]){ { "Cancel", 0, 0, NULL },
                                   { format_CStr(uiTextAction_ColorEscape "Add %d Bookmark%s",
                                                 size_PtrArray(links),
                                                 plural), 0, 0, "bookmark.links" } },
                    2);
            }
            else {
                iConstForEach(PtrArray, j, links) {
                    const iGmRun *run = j.ptr;
                    add_Bookmarks(bookmarks_App(),
                                  linkUrl_GmDocument(d->doc, run->linkId),
                                  collect_String(newRange_String(run->text)),
                                  NULL,
                                  0x1f588 /* pin */);
                }
                postCommand_App("bookmarks.changed");
            }
        }
        else {
            makeMessage_Widget(uiHeading_ColorEscape "IMPORT BOOKMARKS",
                               "All links on this page are already bookmarked.");
        }
        return iTrue;
    }
    else if (equalWidget_Command(cmd, w, "menu.closed")) {
        updateHover_DocumentWidget_(d, mouseCoord_Window(get_Window()));
    }
    else if (equal_Command(cmd, "document.autoreload")) {
        if (d->mod.reloadInterval) {
            if (!isValid_Time(&d->sourceTime) || elapsedSeconds_Time(&d->sourceTime) >=
                    seconds_ReloadInterval_(d->mod.reloadInterval)) {
                postCommand_Widget(w, "document.reload");
            }
        }
    }
    else if (equal_Command(cmd, "document.autoreload.menu") && document_App() == d) {
        iWidget *dlg = makeQuestion_Widget(uiTextAction_ColorEscape "AUTO-RELOAD",
                                           "Select the auto-reload interval for this tab.",
                                           (iMenuItem[]){ { "Cancel", 0, 0, NULL } },
                                           1);
        for (int i = 0; i < max_ReloadInterval; ++i) {
            insertChildAfterFlags_Widget(
                dlg,
                iClob(new_LabelWidget(label_ReloadInterval_(i),
                                      format_CStr("document.autoreload.set arg:%d", i))),
                i + 1,
                resizeToParentWidth_WidgetFlag |
                    ((int) d->mod.reloadInterval == i ? selected_WidgetFlag : 0));
        }
        arrange_Widget(dlg);
        return iTrue;
    }
    else if (equal_Command(cmd, "document.autoreload.set") && document_App() == d) {
        d->mod.reloadInterval = arg_Command(cmd);
    }
    return iFalse;
}

static iRect runRect_DocumentWidget_(const iDocumentWidget *d, const iGmRun *run) {
    const iRect docBounds = documentBounds_DocumentWidget_(d);
    return moved_Rect(run->bounds, addY_I2(topLeft_Rect(docBounds), -value_Anim(&d->scrollY)));
}

static void setGrabbedPlayer_DocumentWidget_(iDocumentWidget *d, const iGmRun *run) {
    if (run && run->mediaType == audio_GmRunMediaType) {
        iPlayer *plr = audioPlayer_Media(media_GmDocument(d->doc), run->mediaId);
        setFlags_Player(plr, volumeGrabbed_PlayerFlag, iTrue);
        d->grabbedStartVolume = volume_Player(plr);
        d->grabbedPlayer      = run;
        refresh_Widget(d);
    }
    else if (d->grabbedPlayer) {
        setFlags_Player(
            audioPlayer_Media(media_GmDocument(d->doc), d->grabbedPlayer->mediaId),
            volumeGrabbed_PlayerFlag,
            iFalse);
        d->grabbedPlayer = NULL;
        refresh_Widget(d);
    }
    else {
        iAssert(iFalse);
    }
}

static iBool processMediaEvents_DocumentWidget_(iDocumentWidget *d, const SDL_Event *ev) {
    if (ev->type != SDL_MOUSEBUTTONDOWN && ev->type != SDL_MOUSEBUTTONUP &&
        ev->type != SDL_MOUSEMOTION) {
        return iFalse;
    }
    if (ev->type == SDL_MOUSEBUTTONDOWN || ev->type == SDL_MOUSEBUTTONUP) {
        if (ev->button.button != SDL_BUTTON_LEFT) {
            return iFalse;
        }
    }
    if (d->grabbedPlayer) {
        /* Updated in the drag. */
        return iFalse;
    }
    const iInt2 mouse = init_I2(ev->button.x, ev->button.y);
    iConstForEach(PtrArray, i, &d->visibleMedia) {
        const iGmRun *run  = i.ptr;
        if (run->mediaType != audio_GmRunMediaType) {
            continue;
        }
        const iRect rect = runRect_DocumentWidget_(d, run);
        iPlayer *   plr  = audioPlayer_Media(media_GmDocument(d->doc), run->mediaId);
        if (contains_Rect(rect, mouse)) {
            iPlayerUI ui;
            init_PlayerUI(&ui, plr, rect);
            if (ev->type == SDL_MOUSEBUTTONDOWN && flags_Player(plr) & adjustingVolume_PlayerFlag &&
                contains_Rect(adjusted_Rect(ui.volumeAdjustRect,
                                            zero_I2(),
                                            init_I2(-height_Rect(ui.volumeAdjustRect), 0)),
                              mouse)) {
                setGrabbedPlayer_DocumentWidget_(d, run);
                processEvent_Click(&d->click, ev);
                /* The rest is done in the DocumentWidget click responder. */
                refresh_Widget(d);
                return iTrue;
            }
            else if (ev->type == SDL_MOUSEBUTTONDOWN || ev->type == SDL_MOUSEMOTION) {
                refresh_Widget(d);
                return iTrue;
            }
            if (contains_Rect(ui.playPauseRect, mouse)) {
                setPaused_Player(plr, !isPaused_Player(plr));
                animateMedia_DocumentWidget_(d);
                return iTrue;
            }
            else if (contains_Rect(ui.rewindRect, mouse)) {
                if (isStarted_Player(plr) && time_Player(plr) > 0.5f) {
                    stop_Player(plr);
                    start_Player(plr);
                    setPaused_Player(plr, iTrue);
                }
                refresh_Widget(d);
                return iTrue;
            }
            else if (contains_Rect(ui.volumeRect, mouse)) {
                setFlags_Player(plr,
                                adjustingVolume_PlayerFlag,
                                !(flags_Player(plr) & adjustingVolume_PlayerFlag));
                animateMedia_DocumentWidget_(d);
                refresh_Widget(d);
                return iTrue;
            }
            else if (contains_Rect(ui.menuRect, mouse)) {
                /* TODO: Add menu items for:
                   - output device
                   - Save to Downloads
                */
                if (d->playerMenu) {
                    destroy_Widget(d->playerMenu);
                    d->playerMenu = NULL;
                    return iTrue;
                }
                d->playerMenu = makeMenu_Widget(
                    as_Widget(d),
                    (iMenuItem[]){
                        { cstrCollect_String(metadataLabel_Player(plr)), 0, 0, NULL },
                    },
                    1);
                openMenu_Widget(d->playerMenu,
                                localCoord_Widget(constAs_Widget(d), bottomLeft_Rect(ui.menuRect)));
                return iTrue;
            }
        }
    }
    return iFalse;
}

static size_t linkOrdinalFromKey_DocumentWidget_(const iDocumentWidget *d, int key) {
    size_t ord = iInvalidPos;
    if (d->ordinalMode == numbersAndAlphabet_DocumentLinkOrdinalMode) {
        if (key >= '1' && key <= '9') {
            return key - '1';
        }
        if (key < 'a' || key > 'z') {
            return iInvalidPos;
        }
        ord = key - 'a' + 9;
#if defined (iPlatformApple)
        /* Skip keys that would conflict with default system shortcuts: hide, minimize, quit, close. */
        if (key == 'h' || key == 'm' || key == 'q' || key == 'w') {
            return iInvalidPos;
        }
        if (key > 'h') ord--;
        if (key > 'm') ord--;
        if (key > 'q') ord--;
        if (key > 'w') ord--;
#endif
    }
    else {
        iForIndices(i, homeRowKeys_) {
            if (homeRowKeys_[i] == key) {
                return i;
            }
        }
    }
    return ord;
}

static iChar linkOrdinalChar_DocumentWidget_(const iDocumentWidget *d, size_t ord) {
    if (d->ordinalMode == numbersAndAlphabet_DocumentLinkOrdinalMode) {
        if (ord < 9) {
            return 0x278a + ord;
        }
#if defined (iPlatformApple)
        if (ord < 9 + 22) {
            int key = 'a' + ord - 9;
            if (key >= 'h') key++;
            if (key >= 'm') key++;
            if (key >= 'q') key++;
            if (key >= 'w') key++;
            return 0x24b6 + key - 'a';
        }
#else
        if (ord < 9 + 26) {
            return 0x24b6 + ord - 9;
        }
#endif
    }
    else {
        if (ord < iElemCount(homeRowKeys_)) {
            return 0x24b6 + homeRowKeys_[ord] - 'a';
        }
    }
    return 0;
}

static iBool processEvent_DocumentWidget_(iDocumentWidget *d, const SDL_Event *ev) {
    iWidget *w = as_Widget(d);
    if (isMetricsChange_UserEvent(ev)) {
        updateSize_DocumentWidget(d);
    }
    else if (ev->type == SDL_USEREVENT && ev->user.code == command_UserEventCode) {
        if (!handleCommand_DocumentWidget_(d, command_UserEvent(ev))) {
            /* Base class commands. */
            return processEvent_Widget(w, ev);
        }
        return iTrue;
    }
    if (ev->type == SDL_KEYDOWN) {
        const int key = ev->key.keysym.sym;
        if ((d->flags & showLinkNumbers_DocumentWidgetFlag) &&
            ((key >= '1' && key <= '9') || (key >= 'a' && key <= 'z'))) {
            const size_t ord = linkOrdinalFromKey_DocumentWidget_(d, key) + d->ordinalBase;
            iConstForEach(PtrArray, i, &d->visibleLinks) {
                if (ord == iInvalidPos) break;
                const iGmRun *run = i.ptr;
                if (run->flags & decoration_GmRunFlag &&
                    visibleLinkOrdinal_DocumentWidget_(d, run->linkId) == ord) {
                    if (d->flags & setHoverViaKeys_DocumentWidgetFlag) {
                        d->hoverLink = run;
                    }
                    else {
                        postCommandf_App("open newtab:%d url:%s",
                                         d->ordinalMode ==
                                                 numbersAndAlphabet_DocumentLinkOrdinalMode
                                             ? openTabMode_Sym(modState_Keys())
                                             : (d->flags & newTabViaHomeKeys_DocumentWidgetFlag ? 1 : 0),
                                         cstr_String(absoluteUrl_String(
                                             d->mod.url, linkUrl_GmDocument(d->doc, run->linkId))));
                    }
                    iChangeFlags(d->flags, showLinkNumbers_DocumentWidgetFlag, iFalse);
                    invalidateVisibleLinks_DocumentWidget_(d);
                    refresh_Widget(d);
                    return iTrue;
                }
            }
        }
        switch (key) {
            case SDLK_ESCAPE:
                if (d->flags & showLinkNumbers_DocumentWidgetFlag && document_App() == d) {
                    iChangeFlags(d->flags, showLinkNumbers_DocumentWidgetFlag, iFalse);
                    invalidateVisibleLinks_DocumentWidget_(d);
                    refresh_Widget(d);
                    return iTrue;
                }
                break;
#if 1
            case SDLK_KP_1:
            case '`': {
                iBlock *seed = new_Block(64);
                for (size_t i = 0; i < 64; ++i) {
                    setByte_Block(seed, i, iRandom(0, 256));
                }
                setThemeSeed_GmDocument(d->doc, seed);
                delete_Block(seed);
                invalidate_DocumentWidget_(d);
                refresh_Widget(w);
                break;
            }
#endif
#if 0
            case '0': {
                extern int enableHalfPixelGlyphs_Text;
                enableHalfPixelGlyphs_Text = !enableHalfPixelGlyphs_Text;
                refresh_Widget(w);
                printf("halfpixel: %d\n", enableHalfPixelGlyphs_Text);
                fflush(stdout);
                break;
            }
#endif
        }
    }
    else if (ev->type == SDL_MOUSEWHEEL && isHover_Widget(w)) {
        /* TODO: Maybe clean this up a bit? Wheel events are used for scrolling
           but they are calculated differently based on device/mouse/trackpad. */
        const iInt2 mouseCoord = mouseCoord_Window(get_Window());
        if (isPerPixel_MouseWheelEvent(&ev->wheel)) {
            stop_Anim(&d->scrollY);
            iInt2 wheel = init_I2(ev->wheel.x, ev->wheel.y);
//#   if defined (iPlatformAppleMobile)
//            wheel.x = -wheel.x;
//#   else
//            /* Wheel mounts are in points. */
//            mulfv_I2(&wheel, get_Window()->pixelRatio);
//            /* Only scroll on one axis at a time. */
//            if (iAbs(wheel.x) > iAbs(wheel.y)) {
//                wheel.y = 0;
//            }
//            else {
//                wheel.x = 0;
//            }
//#   endif
            scroll_DocumentWidget_(d, -wheel.y);
            scrollWideBlock_DocumentWidget_(d, mouseCoord, -wheel.x, 0);
        }
        else {
            /* Traditional mouse wheel. */
//#if defined (iPlatformApple)
//            /* Disregard wheel acceleration applied by the OS. */
//            const int amount = iSign(ev->wheel.y);
//#else
            const int amount = ev->wheel.y;
//#endif
            if (keyMods_Sym(modState_Keys()) == KMOD_PRIMARY) {
                postCommandf_App("zoom.delta arg:%d", amount > 0 ? 10 : -10);
                return iTrue;
            }
            smoothScroll_DocumentWidget_(
                d,
                -3 * amount * lineHeight_Text(paragraph_FontId),
                smoothDuration_DocumentWidget_ *
                    /* accelerated speed for repeated wheelings */
                    (!isFinished_Anim(&d->scrollY) && pos_Anim(&d->scrollY) < 0.25f ? 0.5f : 1.0f));
            scrollWideBlock_DocumentWidget_(
                d, mouseCoord, -3 * ev->wheel.x * lineHeight_Text(paragraph_FontId), 167);
        }
        iChangeFlags(d->flags, noHoverWhileScrolling_DocumentWidgetFlag, iTrue);
        return iTrue;
    }
    else if (ev->type == SDL_MOUSEMOTION) {
        iChangeFlags(d->flags, noHoverWhileScrolling_DocumentWidgetFlag, iFalse);
        const iInt2 mpos = init_I2(ev->motion.x, ev->motion.y);
        if (isVisible_Widget(d->menu)) {
            setCursor_Window(get_Window(), SDL_SYSTEM_CURSOR_ARROW);
        }
        else if (contains_Rect(siteBannerRect_DocumentWidget_(d), mpos)) {
            setCursor_Window(get_Window(), SDL_SYSTEM_CURSOR_HAND);
        }
        else {
            updateHover_DocumentWidget_(d, mpos);
        }
    }
    if (ev->type == SDL_MOUSEBUTTONDOWN) {
        if (ev->button.button == SDL_BUTTON_X1) {
            postCommand_App("navigate.back");
            return iTrue;
        }
        if (ev->button.button == SDL_BUTTON_X2) {
            postCommand_App("navigate.forward");
            return iTrue;
        }
        if (ev->button.button == SDL_BUTTON_MIDDLE && d->hoverLink) {
            postCommandf_App("open newtab:%d url:%s",
                             modState_Keys() & KMOD_SHIFT ? 1 : 2,
                             cstr_String(linkUrl_GmDocument(d->doc, d->hoverLink->linkId)));
            return iTrue;
        }
        if (ev->button.button == SDL_BUTTON_RIGHT &&
            contains_Widget(w, init_I2(ev->button.x, ev->button.y))) {
            if (!d->menu || !isVisible_Widget(d->menu)) {
                d->contextLink = d->hoverLink;
                if (d->menu) {
                    destroy_Widget(d->menu);
                }
                setFocus_Widget(NULL);
                iArray items;
                init_Array(&items, sizeof(iMenuItem));
                if (d->contextLink) {
                    const iString *linkUrl  = linkUrl_GmDocument(d->doc, d->contextLink->linkId);
                    const int      linkFlags = linkFlags_GmDocument(d->doc, d->contextLink->linkId);
                    const iRangecc scheme   = urlScheme_String(linkUrl);
                    const iBool    isGemini = equalCase_Rangecc(scheme, "gemini");
                    iBool          isNative = iFalse;
                    if (willUseProxy_App(scheme) || isGemini ||
                        equalCase_Rangecc(scheme, "finger") ||
                        equalCase_Rangecc(scheme, "gopher")) {
                        isNative = iTrue;
                        /* Regular links that we can open. */
                        pushBackN_Array(
                            &items,
                            (iMenuItem[]){
                                { openTab_Icon " Open Link in New Tab",
                                  0,
                                  0,
                                  format_CStr("!open newtab:1 url:%s", cstr_String(linkUrl)) },
                                { openTabBg_Icon " Open Link in Background Tab",
                                  0,
                                  0,
                                  format_CStr("!open newtab:2 url:%s", cstr_String(linkUrl)) } },
                            2);
                    }
                    else if (!willUseProxy_App(scheme)) {
                        pushBack_Array(
                            &items,
                            &(iMenuItem){ openExt_Icon " Open Link in Default Browser",
                                          0,
                                          0,
                                          format_CStr("!open default:1 url:%s", cstr_String(linkUrl)) });
                    }
                    if (willUseProxy_App(scheme)) {
                        pushBackN_Array(
                            &items,
                            (iMenuItem[]){
                                { "---", 0, 0, NULL },
                                { isGemini ? "Open without Proxy" : openExt_Icon " Open Link in Default Browser",
                                  0,
                                  0,
                                  format_CStr("!open noproxy:1 url:%s", cstr_String(linkUrl)) } },
                            2);
                    }
                    iString *linkLabel = collectNewRange_String(
                        linkLabel_GmDocument(d->doc, d->contextLink->linkId));
                    urlEncodeSpaces_String(linkLabel);
                    pushBackN_Array(&items,
                                    (iMenuItem[]){ { "---", 0, 0, NULL },
                                                   { "Copy Link", 0, 0, "document.copylink" },
                                                   { pin_Icon " Bookmark Link...",
                                                     0,
                                                     0,
                                                     format_CStr("!bookmark.add title:%s url:%s",
                                                                 cstr_String(linkLabel),
                                                                 cstr_String(linkUrl)) },
                                                   },
                                    3);
                    if (isNative && d->contextLink->mediaType != download_GmRunMediaType) {
                        pushBackN_Array(&items, (iMenuItem[]){
                            { "---", 0, 0, NULL },
                            { download_Icon " Download Linked File", 0, 0, "document.downloadlink" },
                        }, 2);
                    }
                    iMediaRequest *mediaReq;
                    if ((mediaReq = findMediaRequest_DocumentWidget_(d, d->contextLink->linkId)) != NULL &&
                        d->contextLink->mediaType != download_GmRunMediaType) {
                        if (isFinished_GmRequest(mediaReq->req)) {
                            pushBack_Array(&items,
                                           &(iMenuItem){ download_Icon " Save to Downloads",
                                                         0,
                                                         0,
                                                         format_CStr("document.media.save link:%u",
                                                                     d->contextLink->linkId) });
                        }
                    }
                }
                else {
                    if (!isEmpty_Range(&d->selectMark)) {
                        pushBackN_Array(
                            &items,
                            (iMenuItem[]){ { "Copy", 0, 0, "copy" }, { "---", 0, 0, NULL } },
                            2);
                    }
                    if (deviceType_App() == desktop_AppDeviceType) {
                        pushBackN_Array(
                            &items,
                            (iMenuItem[]){
                                { "Go Back", navigateBack_KeyShortcut, "navigate.back" },
                            { "Go Forward", navigateForward_KeyShortcut, "navigate.forward" } },
                        2);
                    }
                    pushBackN_Array(
                        &items,
                        (iMenuItem[]){
                            { upArrow_Icon " Go to Parent", navigateParent_KeyShortcut, "navigate.parent" },
                            { upArrowBar_Icon " Go to Root", navigateRoot_KeyShortcut, "navigate.root" },
                            { "---", 0, 0, NULL },
                            { reload_Icon " Reload Page", reload_KeyShortcut, "navigate.reload" },
                            { timer_Icon " Set Auto-Reload...", 0, 0, "document.autoreload.menu" },
                            { "---", 0, 0, NULL },
                            { pin_Icon " Bookmark Page...", SDLK_d, KMOD_PRIMARY, "bookmark.add" },
                            { star_Icon " Subscribe to Page...", subscribeToPage_KeyModifier, "feeds.subscribe" },
                            { "---", 0, 0, NULL },
                            { book_Icon " Import Links as Bookmarks...", 0, 0, "bookmark.links confirm:1" },
                            { "---", 0, 0, NULL },
                            { "Copy Page URL", 0, 0, "document.copylink" } },
                        11);
                    if (isEmpty_Range(&d->selectMark)) {
                        pushBackN_Array(
                            &items,
                            (iMenuItem[]){
                                { "Copy Page Source", 'c', KMOD_PRIMARY, "copy" },
                                { download_Icon " Save to Downloads", SDLK_s, KMOD_PRIMARY, "document.save" } },
                            2);
                    }
                }
                d->menu = makeMenu_Widget(w, data_Array(&items), size_Array(&items));
                deinit_Array(&items);
            }
            processContextMenuEvent_Widget(d->menu, ev, {});
        }
    }
    if (processMediaEvents_DocumentWidget_(d, ev)) {
        return iTrue;
    }
    /* The left mouse button. */
    switch (processEvent_Click(&d->click, ev)) {
        case started_ClickResult:
            iChangeFlags(d->flags, selecting_DocumentWidgetFlag, iFalse);
            return iTrue;
        case drag_ClickResult: {
            if (d->grabbedPlayer) {
                iPlayer *plr =
                    audioPlayer_Media(media_GmDocument(d->doc), d->grabbedPlayer->mediaId);
                iPlayerUI ui;
                init_PlayerUI(&ui, plr, runRect_DocumentWidget_(d, d->grabbedPlayer));
                float off = (float) delta_Click(&d->click).x / (float) width_Rect(ui.volumeSlider);
                setVolume_Player(plr, d->grabbedStartVolume + off);
                refresh_Widget(w);
                return iTrue;
            }
            /* Begin selecting a range of text. */
            if (~d->flags & selecting_DocumentWidgetFlag) {
                setFocus_Widget(NULL); /* TODO: Focus this document? */
                invalidateWideRunsWithNonzeroOffset_DocumentWidget_(d);
                resetWideRuns_DocumentWidget_(d); /* Selections don't support horizontal scrolling. */
                iChangeFlags(d->flags, selecting_DocumentWidgetFlag, iTrue);
                d->selectMark.start = d->selectMark.end =
                    sourceLoc_DocumentWidget_(d, d->click.startPos);
                refresh_Widget(w);
            }
            const char *loc = sourceLoc_DocumentWidget_(d, pos_Click(&d->click));
            if (!d->selectMark.start) {
                d->selectMark.start = d->selectMark.end = loc;
            }
            else if (loc) {
                d->selectMark.end = loc;
            }
//            printf("mark %zu ... %zu\n", d->selectMark.start - cstr_String(source_GmDocument(d->doc)),
//                   d->selectMark.end - cstr_String(source_GmDocument(d->doc)));
//            fflush(stdout);
            refresh_Widget(w);
            return iTrue;
        }
        case finished_ClickResult:
            if (d->grabbedPlayer) {
                setGrabbedPlayer_DocumentWidget_(d, NULL);
                return iTrue;
            }
            if (isVisible_Widget(d->menu)) {
                closeMenu_Widget(d->menu);
            }
            if (!isMoved_Click(&d->click)) {
                setFocus_Widget(NULL);
                if (d->hoverLink) {
                    const iGmLinkId linkId = d->hoverLink->linkId;
                    const int linkFlags = linkFlags_GmDocument(d->doc, linkId);
                    iAssert(linkId);
                    /* Media links are opened inline by default. */
                    if (isMediaLink_GmDocument(d->doc, linkId)) {
                        if (linkFlags & content_GmLinkFlag && linkFlags & permanent_GmLinkFlag) {
                            /* We have the content and it cannot be dismissed, so nothing
                               further to do. */
                            return iTrue;
                        }
                        if (!requestMedia_DocumentWidget_(d, linkId, iTrue)) {
                            if (linkFlags & content_GmLinkFlag) {
                                /* Dismiss shown content on click. */
                                setData_Media(media_GmDocument(d->doc),
                                              linkId,
                                              NULL,
                                              NULL,
                                              allowHide_MediaFlag);
                                /* Cancel a partially received request. */ {
                                    iMediaRequest *req = findMediaRequest_DocumentWidget_(d, linkId);
                                    if (!isFinished_GmRequest(req->req)) {
                                        cancel_GmRequest(req->req);
                                        removeMediaRequest_DocumentWidget_(d, linkId);
                                        /* Note: Some of the audio IDs have changed now, layout must
                                           be redone. */
                                    }
                                }
                                redoLayout_GmDocument(d->doc);
                                d->hoverLink = NULL;
                                scroll_DocumentWidget_(d, 0);
                                updateVisible_DocumentWidget_(d);
                                invalidate_DocumentWidget_(d);
                                refresh_Widget(w);
                                return iTrue;
                            }
                            else {
                                /* Show the existing content again if we have it. */
                                iMediaRequest *req = findMediaRequest_DocumentWidget_(d, linkId);
                                if (req) {
                                    setData_Media(media_GmDocument(d->doc),
                                                  linkId,
                                                  meta_GmRequest(req->req),
                                                  body_GmRequest(req->req),
                                                  allowHide_MediaFlag);
                                    redoLayout_GmDocument(d->doc);
                                    updateVisible_DocumentWidget_(d);
                                    invalidate_DocumentWidget_(d);
                                    refresh_Widget(w);
                                    return iTrue;
                                }
                            }
                        }
                        refresh_Widget(w);
                    }
                    else if (linkFlags & supportedProtocol_GmLinkFlag) {
                        postCommandf_App("open newtab:%d url:%s",
                                         openTabMode_Sym(modState_Keys()),
                                         cstr_String(absoluteUrl_String(
                                             d->mod.url, linkUrl_GmDocument(d->doc, linkId))));
                    }
                    else {
                        const iString *url = absoluteUrl_String(
                            d->mod.url, linkUrl_GmDocument(d->doc, linkId));
                        makeQuestion_Widget(
                            uiTextCaution_ColorEscape "OPEN LINK",
                            format_CStr(
                                "Open this link in the default browser?\n" uiTextAction_ColorEscape
                                "%s",
                                cstr_String(url)),
                            (iMenuItem[]){
                                { "Cancel", 0, 0, NULL },
                                { uiTextCaution_ColorEscape "Open Link",
                                  0, 0, format_CStr("!open default:1 url:%s", cstr_String(url)) } },
                            2);
                    }
                }
                if (d->selectMark.start) {
                    d->selectMark = iNullRange;
                    refresh_Widget(w);
                }
                /* Clicking on the top/side banner navigates to site root. */
                const iRect banRect = siteBannerRect_DocumentWidget_(d);
                if (contains_Rect(banRect, pos_Click(&d->click))) {
                    /* Clicking on a warning? */
                    if (bannerType_DocumentWidget_(d) == certificateWarning_GmDocumentBanner &&
                        pos_Click(&d->click).y - top_Rect(banRect) >
                            lineHeight_Text(banner_FontId) * 2) {
                        postCommand_App("document.info");
                    }
                    else {
                        postCommand_Widget(d, "navigate.root");
                    }
                }
            }
            return iTrue;
        case double_ClickResult:
        case aborted_ClickResult:
            if (d->grabbedPlayer) {
                setGrabbedPlayer_DocumentWidget_(d, NULL);
                return iTrue;
            }
            return iTrue;
        default:
            break;
    }
    return processEvent_Widget(w, ev);
}

iDeclareType(DrawContext)

struct Impl_DrawContext {
    const iDocumentWidget *widget;
    iRect widgetBounds;
    iInt2 viewPos; /* document area origin */
    iPaint paint;
    iBool inSelectMark;
    iBool inFoundMark;
    iBool showLinkNumbers;
};

static void fillRange_DrawContext_(iDrawContext *d, const iGmRun *run, enum iColorId color,
                                   iRangecc mark, iBool *isInside) {
    if (mark.start > mark.end) {
        /* Selection may be done in either direction. */
        iSwap(const char *, mark.start, mark.end);
    }
    if ((!*isInside && (contains_Range(&run->text, mark.start) || mark.start == run->text.end)) ||
        *isInside) {
        int x = 0;
        if (!*isInside) {
            x = advanceRange_Text(run->font, (iRangecc){ run->text.start, mark.start }).x;
        }
        int w = width_Rect(run->visBounds) - x;
        if (contains_Range(&run->text, mark.end) || run->text.end == mark.end) {
            w = advanceRange_Text(run->font,
                                  !*isInside ? mark : (iRangecc){ run->text.start, mark.end }).x;
            *isInside = iFalse;
        }
        else {
            *isInside = iTrue; /* at least until the next run */
        }
        if (w > width_Rect(run->visBounds) - x) {
            w = width_Rect(run->visBounds) - x;
        }
        const iInt2 visPos =
            add_I2(run->bounds.pos, addY_I2(d->viewPos, -value_Anim(&d->widget->scrollY)));
        fillRect_Paint(&d->paint, (iRect){ addX_I2(visPos, x),
                                           init_I2(w, height_Rect(run->bounds)) }, color);
    }
    /* Link URLs are not part of the visible document, so they are ignored above. Handle
       these ranges as a special case. */
    if (run->linkId && run->flags & decoration_GmRunFlag) {
        const iRangecc url = linkUrlRange_GmDocument(d->widget->doc, run->linkId);
        if (contains_Range(&url, mark.start) &&
            (contains_Range(&url, mark.end) || url.end == mark.end)) {
            fillRect_Paint(
                &d->paint,
                moved_Rect(run->visBounds, addY_I2(d->viewPos, -value_Anim(&d->widget->scrollY))),
                color);
        }
    }
}

static void drawMark_DrawContext_(void *context, const iGmRun *run) {
    iDrawContext *d = context;
    if (run->mediaType == none_GmRunMediaType) {
        fillRange_DrawContext_(d, run, uiMatching_ColorId, d->widget->foundMark, &d->inFoundMark);
        fillRange_DrawContext_(d, run, uiMarked_ColorId, d->widget->selectMark, &d->inSelectMark);
    }
}

static void drawBannerRun_DrawContext_(iDrawContext *d, const iGmRun *run, iInt2 visPos) {
    const iGmDocument *doc  = d->widget->doc;
    const iChar        icon = siteIcon_GmDocument(doc);
    iString            str;
    init_String(&str);
    iInt2 bpos = add_I2(visPos, init_I2(0, lineHeight_Text(banner_FontId) / 2));
    if (icon) {
        appendChar_String(&str, icon);
        const iRect iconRect = visualBounds_Text(run->font, range_String(&str));
        drawRange_Text(
            run->font,
            addY_I2(bpos, -mid_Rect(iconRect).y + lineHeight_Text(run->font) / 2),
            tmBannerIcon_ColorId,
            range_String(&str));
        bpos.x += right_Rect(iconRect) + 3 * gap_Text;
    }
    drawRange_Text(run->font,
                   bpos,
                   tmBannerTitle_ColorId,
                   bannerText_DocumentWidget_(d->widget));
    if (bannerType_GmDocument(doc) == certificateWarning_GmDocumentBanner) {
        const int domainHeight = lineHeight_Text(banner_FontId) * 2;
        iRect rect = { add_I2(visPos, init_I2(0, domainHeight)),
                       addY_I2(run->visBounds.size, -domainHeight - lineHeight_Text(uiContent_FontId)) };
        format_String(&str, "UNTRUSTED CERTIFICATE");
        const int certFlags = d->widget->certFlags;
        if (certFlags & timeVerified_GmCertFlag && certFlags & domainVerified_GmCertFlag) {
            iUrl parts;
            init_Url(&parts, d->widget->mod.url);
            const iTime oldUntil = domainValidUntil_GmCerts(certs_App(), parts.host);
            iDate exp;
            init_Date(&exp, &oldUntil);
            iTime now;
            initCurrent_Time(&now);
            const int days = secondsSince_Time(&oldUntil, &now) / 3600 / 24;
            if (days <= 30) {
                appendFormat_String(&str,
                                    "\nThe received certificate may have been recently renewed "
                                    "\u2014 it is for the correct domain and has not expired. "
                                    "The currently trusted certificate will expire on %s, "
                                    "in %d days.",
                                    cstrCollect_String(format_Date(&exp, "%Y-%m-%d")),
                                    days);
            }
            else {
                appendFormat_String(&str, "\nThe received certificate is valid but different than "
                                          "the one we trust.");
            }
        }
        else if (certFlags & domainVerified_GmCertFlag) {
            appendFormat_String(&str, "\nThe received certificate has expired on %s.",
                                cstrCollect_String(format_Date(&d->widget->certExpiry, "%Y-%m-%d")));
        }
        else if (certFlags & timeVerified_GmCertFlag) {
            appendFormat_String(&str, "\nThe received certificate is for the wrong domain (%s). "
                                "This may be a server configuration problem.",
                                cstr_String(d->widget->certSubject));
        }
        else {
            appendFormat_String(&str, "\nThe received certificate is expired AND for the "
                                      "wrong domain.");
        }
        const iInt2 dims = advanceWrapRange_Text(
            uiContent_FontId, width_Rect(rect) - 16 * gap_UI, range_String(&str));
        const int warnHeight = run->visBounds.size.y - domainHeight;
        const int yOff = (lineHeight_Text(uiLabelLarge_FontId) -
                          lineHeight_Text(uiContent_FontId)) / 2;
        const iRect bgRect =
            init_Rect(0, visPos.y + domainHeight, d->widgetBounds.size.x, warnHeight);
        fillRect_Paint(&d->paint, bgRect, orange_ColorId);
        if (!isDark_ColorTheme(colorTheme_App())) {
            drawHLine_Paint(&d->paint,
                            topLeft_Rect(bgRect), width_Rect(bgRect), tmBannerTitle_ColorId);
            drawHLine_Paint(&d->paint,
                            bottomLeft_Rect(bgRect), width_Rect(bgRect), tmBannerTitle_ColorId);
        }
        const int fg = black_ColorId;
        adjustEdges_Rect(&rect, warnHeight / 2 - dims.y / 2 - yOff, 0, 0, 0);
        bpos = topLeft_Rect(rect);
        draw_Text(uiLabelLarge_FontId, bpos, fg, "\u26a0");
        adjustEdges_Rect(&rect, 0, -8 * gap_UI, 0, 8 * gap_UI);
        drawWrapRange_Text(uiContent_FontId,
                           addY_I2(topLeft_Rect(rect), yOff),
                           width_Rect(rect),
                           fg,
                           range_String(&str));
    }
    deinit_String(&str);
}

static void drawRun_DrawContext_(void *context, const iGmRun *run) {
    iDrawContext *d      = context;
    const iInt2   origin = d->viewPos;
    if (run->mediaType == image_GmRunMediaType) {
        SDL_Texture *tex = imageTexture_Media(media_GmDocument(d->widget->doc), run->mediaId);
        const iRect dst = moved_Rect(run->visBounds, origin);
        if (tex) {
            fillRect_Paint(&d->paint, dst, tmBackground_ColorId); /* in case the image has alpha */
            SDL_RenderCopy(d->paint.dst->render, tex, NULL,
                           &(SDL_Rect){ dst.pos.x, dst.pos.y, dst.size.x, dst.size.y });
        }
        else {
            drawRect_Paint(&d->paint, dst, tmQuoteIcon_ColorId);
            drawCentered_Text(uiLabel_FontId,
                              dst,
                              iFalse,
                              tmQuote_ColorId,
                              explosion_Icon "  Error Loading Image");
        }
        return;
    }
    else if (run->mediaType) {
        /* Media UIs are drawn afterwards as a dynamic overlay. */
        return;
    }
    enum iColorId      fg  = run->color;
    const iGmDocument *doc = d->widget->doc;
    const iBool        isHover =
        (run->linkId && d->widget->hoverLink && run->linkId == d->widget->hoverLink->linkId &&
         ~run->flags & decoration_GmRunFlag);
    const iInt2 visPos = addX_I2(add_I2(run->visBounds.pos, origin),
                                 /* Preformatted runs can be scrolled. */
                                 runOffset_DocumentWidget_(d->widget, run));
    fillRect_Paint(&d->paint, (iRect){ visPos, run->visBounds.size }, tmBackground_ColorId);
    if (run->linkId && ~run->flags & decoration_GmRunFlag) {
        fg = linkColor_GmDocument(doc, run->linkId, isHover ? textHover_GmLinkPart : text_GmLinkPart);
        if (linkFlags_GmDocument(doc, run->linkId) & content_GmLinkFlag) {
            fg = linkColor_GmDocument(doc, run->linkId, textHover_GmLinkPart); /* link is inactive */
        }
    }
    if (run->flags & siteBanner_GmRunFlag) {
        /* Banner background. */
        iRect bannerBack = initCorners_Rect(topLeft_Rect(d->widgetBounds),
                                            init_I2(right_Rect(bounds_Widget(constAs_Widget(d->widget))),
                                                    visPos.y + height_Rect(run->visBounds)));
        fillRect_Paint(&d->paint, bannerBack, tmBannerBackground_ColorId);
        drawBannerRun_DrawContext_(d, run, visPos);
    }
    else {
        if (d->showLinkNumbers && run->linkId && run->flags & decoration_GmRunFlag) {
            const size_t ord = visibleLinkOrdinal_DocumentWidget_(d->widget, run->linkId);
            if (ord >= d->widget->ordinalBase) {
                const iChar ordChar =
                    linkOrdinalChar_DocumentWidget_(d->widget, ord - d->widget->ordinalBase);
                if (ordChar) {
                    drawString_Text(run->font,
                                    init_I2(d->viewPos.x - gap_UI / 3, visPos.y),
                                    tmQuote_ColorId,
                                    collect_String(newUnicodeN_String(&ordChar, 1)));
                    goto runDrawn;
                }
            }
        }
        if (run->flags & quoteBorder_GmRunFlag) {
            drawVLine_Paint(&d->paint,
                            addX_I2(visPos, -gap_Text * 5 / 2),
                            height_Rect(run->visBounds),
                            tmQuoteIcon_ColorId);
        }
        drawBoundRange_Text(run->font, visPos, width_Rect(run->bounds), fg, run->text);
    runDrawn:;
    }
    /* Presentation of links. */
    if (run->linkId && ~run->flags & decoration_GmRunFlag) {
        const int metaFont = paragraph_FontId;
        /* TODO: Show status of an ongoing media request. */
        const int flags = linkFlags_GmDocument(doc, run->linkId);
        const iRect linkRect = moved_Rect(run->visBounds, origin);
        iMediaRequest *mr = NULL;
        /* Show metadata about inline content. */
        if (flags & content_GmLinkFlag && run->flags & endOfLine_GmRunFlag) {
            fg = linkColor_GmDocument(doc, run->linkId, textHover_GmLinkPart);
            iString text;
            init_String(&text);
            iMediaId imageId = linkImage_GmDocument(doc, run->linkId);
            iMediaId audioId = !imageId ? linkAudio_GmDocument(doc, run->linkId) : 0;
            iMediaId downloadId = !imageId && !audioId ?
                findLinkDownload_Media(constMedia_GmDocument(doc), run->linkId) : 0;
            iAssert(imageId || audioId || downloadId);
            if (imageId) {
                iAssert(!isEmpty_Rect(run->bounds));
                iGmMediaInfo info;
                imageInfo_Media(constMedia_GmDocument(doc), imageId, &info);
                const iInt2 imgSize = imageSize_Media(constMedia_GmDocument(doc), imageId);
                format_String(&text, "%s \u2014 %d x %d \u2014 %.1fMB",
                              info.type, imgSize.x, imgSize.y, info.numBytes / 1.0e6f);
            }
            else if (audioId) {
                iGmMediaInfo info;
                audioInfo_Media(constMedia_GmDocument(doc), audioId, &info);
                format_String(&text, "%s", info.type);
            }
            else if (downloadId) {
                iGmMediaInfo info;
                downloadInfo_Media(constMedia_GmDocument(doc), downloadId, &info);
                format_String(&text, "%s", info.type);
            }
            if (findMediaRequest_DocumentWidget_(d->widget, run->linkId)) {
                appendFormat_String(
                    &text, "  %s" close_Icon, isHover ? escape_Color(tmLinkText_ColorId) : "");
            }
            const iInt2 size = measureRange_Text(metaFont, range_String(&text));
            fillRect_Paint(
                &d->paint,
                (iRect){ add_I2(origin, addX_I2(topRight_Rect(run->bounds), -size.x - gap_UI)),
                         addX_I2(size, 2 * gap_UI) },
                tmBackground_ColorId);
            drawAlign_Text(metaFont,
                           add_I2(topRight_Rect(run->bounds), origin),
                           fg,
                           right_Alignment,
                           "%s", cstr_String(&text));
            deinit_String(&text);
        }
        else if (run->flags & endOfLine_GmRunFlag &&
                 (mr = findMediaRequest_DocumentWidget_(d->widget, run->linkId)) != NULL) {
            if (!isFinished_GmRequest(mr->req)) {
                draw_Text(metaFont,
                          topRight_Rect(linkRect),
                          tmInlineContentMetadata_ColorId,
                          " \u2014 Fetching\u2026 (%.1f MB)",
                          (float) bodySize_GmRequest(mr->req) / 1.0e6f);
            }
        }
        else if (isHover) {
            const iGmLinkId linkId = d->widget->hoverLink->linkId;
            const iString * url    = linkUrl_GmDocument(doc, linkId);
            const int       flags  = linkFlags_GmDocument(doc, linkId);
            iUrl parts;
            init_Url(&parts, url);
            fg                    = linkColor_GmDocument(doc, linkId, textHover_GmLinkPart);
            const iBool showHost  = (flags & humanReadable_GmLinkFlag &&
                                    (!isEmpty_Range(&parts.host) || flags & mailto_GmLinkFlag));
            const iBool showImage = (flags & imageFileExtension_GmLinkFlag) != 0;
            const iBool showAudio = (flags & audioFileExtension_GmLinkFlag) != 0;
            iString str;
            init_String(&str);
            /* Show scheme and host. */
            if (run->flags & endOfLine_GmRunFlag &&
                (flags & (imageFileExtension_GmLinkFlag | audioFileExtension_GmLinkFlag) ||
                 showHost)) {
                format_String(&str,
                              " \u2014%s%s%s\r%c%s",
                              showHost ? " " : "",
                              showHost ? (flags & mailto_GmLinkFlag
                                              ? cstr_String(url)
                                              : ~flags & gemini_GmLinkFlag
                                                    ? format_CStr("%s://%s",
                                                                  cstr_Rangecc(parts.scheme),
                                                                  cstr_Rangecc(parts.host))
                                                    : cstr_Rangecc(parts.host))
                                       : "",
                              showHost && (showImage || showAudio) ? " \u2014" : "",
                              showImage || showAudio
                                  ? asciiBase_ColorEscape + fg
                                  : (asciiBase_ColorEscape +
                                     linkColor_GmDocument(doc, run->linkId, domain_GmLinkPart)),
                              showImage ? " View Image \U0001f5bb"
                                        : showAudio ? " Play Audio \U0001f3b5" : "");
            }
            if (run->flags & endOfLine_GmRunFlag && flags & visited_GmLinkFlag) {
                iDate date;
                init_Date(&date, linkTime_GmDocument(doc, run->linkId));
                appendFormat_String(&str,
                                    " \u2014 %s%s",
                                    escape_Color(linkColor_GmDocument(doc, run->linkId,
                                                                      visited_GmLinkPart)),
                                    cstr_String(collect_String(format_Date(&date, "%b %d"))));
            }
            if (!isEmpty_String(&str)) {
                const iInt2 textSize = measure_Text(metaFont, cstr_String(&str));
                int tx = topRight_Rect(linkRect).x;
                const char *msg = cstr_String(&str);
                if (tx + textSize.x > right_Rect(d->widgetBounds)) {
                    tx = right_Rect(d->widgetBounds) - textSize.x;
                    fillRect_Paint(&d->paint, (iRect){ init_I2(tx, top_Rect(linkRect)), textSize },
                                   uiBackground_ColorId);
                    msg += 4; /* skip the space and dash */
                    tx += measure_Text(metaFont, " \u2014").x / 2;
                }
                drawAlign_Text(metaFont,
                               init_I2(tx, top_Rect(linkRect)),
                               linkColor_GmDocument(doc, run->linkId, domain_GmLinkPart),
                               left_Alignment,
                               "%s",
                               msg);
                deinit_String(&str);
            }
        }
    }
//    drawRect_Paint(&d->paint, (iRect){ visPos, run->bounds.size }, green_ColorId);
//    drawRect_Paint(&d->paint, (iRect){ visPos, run->visBounds.size }, red_ColorId);
}

static int drawSideRect_(iPaint *p, iRect rect) {
    int bg = tmBannerBackground_ColorId;
    int fg = tmBannerIcon_ColorId;
    if (equal_Color(get_Color(bg), get_Color(tmBackground_ColorId))) {
        bg = tmBannerIcon_ColorId;
        fg = tmBannerBackground_ColorId;
    }
    fillRect_Paint(p, rect, bg);
    return fg;
}

static int sideElementAvailWidth_DocumentWidget_(const iDocumentWidget *d) {
    return left_Rect(documentBounds_DocumentWidget_(d)) -
           left_Rect(bounds_Widget(constAs_Widget(d))) - 2 * d->pageMargin * gap_UI;
}

static iBool isSideHeadingVisible_DocumentWidget_(const iDocumentWidget *d) {
    return sideElementAvailWidth_DocumentWidget_(d) >= lineHeight_Text(banner_FontId) * 4.5f;
}

static void updateSideIconBuf_DocumentWidget_(iDocumentWidget *d) {
    if (d->sideIconBuf) {
        SDL_DestroyTexture(d->sideIconBuf);
        d->sideIconBuf = NULL;
    }
    const iGmRun *banner = siteBanner_GmDocument(d->doc);
    if (!banner) {
        return;
    }
    const int   margin           = gap_UI * d->pageMargin;
    const int   minBannerSize    = lineHeight_Text(banner_FontId) * 2;
    const iChar icon             = siteIcon_GmDocument(d->doc);
    const int   avail            = sideElementAvailWidth_DocumentWidget_(d) - margin;
    iBool       isHeadingVisible = isSideHeadingVisible_DocumentWidget_(d);
    /* Determine the required size. */
    iInt2 bufSize = init1_I2(minBannerSize);
    if (isHeadingVisible) {
        const iInt2 headingSize = advanceWrapRange_Text(heading3_FontId, avail,
                                                        currentHeading_DocumentWidget_(d));
        if (headingSize.x > 0) {
            bufSize.y += gap_Text + headingSize.y;
            bufSize.x = iMax(bufSize.x, headingSize.x);
        }
        else {
            isHeadingVisible = iFalse;
        }
    }
    SDL_Renderer *render = renderer_Window(get_Window());
    d->sideIconBuf = SDL_CreateTexture(render,
                                       SDL_PIXELFORMAT_RGBA4444,
                                       SDL_TEXTUREACCESS_STATIC | SDL_TEXTUREACCESS_TARGET,
                                       bufSize.x, bufSize.y);
    iPaint p;
    init_Paint(&p);
    beginTarget_Paint(&p, d->sideIconBuf);
    SDL_SetRenderDrawColor(render, 0, 0, 0, 0);
    SDL_RenderClear(render);
    const iRect iconRect = { zero_I2(), init1_I2(minBannerSize) };
    int fg = drawSideRect_(&p, iconRect);
    iString str;
    initUnicodeN_String(&str, &icon, 1);
    drawCentered_Text(banner_FontId, iconRect, iTrue, fg, "%s", cstr_String(&str));
    deinit_String(&str);
    if (isHeadingVisible) {
        iRangecc    text = currentHeading_DocumentWidget_(d);
        iInt2       pos  = addY_I2(bottomLeft_Rect(iconRect), gap_Text);
        const int   font = heading3_FontId;
        drawWrapRange_Text(font, pos, avail, tmBannerSideTitle_ColorId, text);
    }
    endTarget_Paint(&p);
    SDL_SetTextureBlendMode(d->sideIconBuf, SDL_BLENDMODE_BLEND);
}

static void drawSideElements_DocumentWidget_(const iDocumentWidget *d) {
    const iWidget *w         = constAs_Widget(d);
    const iRect    bounds    = bounds_Widget(w);
    const iRect    docBounds = documentBounds_DocumentWidget_(d);
    const int      margin    = gap_UI * d->pageMargin;
    float          opacity   = value_Anim(&d->sideOpacity);
    const int      avail     = left_Rect(docBounds) - left_Rect(bounds) - 2 * margin;
    iPaint      p;
    init_Paint(&p);
    setClip_Paint(&p, bounds);
    /* Side icon and current heading. */
    if (prefs_App()->sideIcon && opacity > 0 && d->sideIconBuf) {
        const iInt2 texSize = size_SDLTexture(d->sideIconBuf);
        if (avail > texSize.x) {
            const int minBannerSize = lineHeight_Text(banner_FontId) * 2;
            iInt2 pos = addY_I2(add_I2(topLeft_Rect(bounds), init_I2(margin, 0)),
                                height_Rect(bounds) / 2 - minBannerSize / 2 -
                                    (texSize.y > minBannerSize
                                         ? (gap_Text + lineHeight_Text(heading3_FontId)) / 2
                                         : 0));
            SDL_SetTextureAlphaMod(d->sideIconBuf, 255 * opacity);
            SDL_RenderCopy(renderer_Window(get_Window()),
                           d->sideIconBuf, NULL,
                           &(SDL_Rect){ pos.x, pos.y, texSize.x, texSize.y });
        }
    }
    /* Reception timestamp. */
    if (d->timestampBuf && d->timestampBuf->size.x <= avail) {
        draw_TextBuf(
            d->timestampBuf,
            add_I2(
                bottomLeft_Rect(bounds),
                init_I2(margin,
                        -margin + -d->timestampBuf->size.y +
                            iMax(0, scrollMax_DocumentWidget_(d) - value_Anim(&d->scrollY)))),
            tmQuoteIcon_ColorId);
    }
    unsetClip_Paint(&p);
}

static void drawMedia_DocumentWidget_(const iDocumentWidget *d, iPaint *p) {
    iConstForEach(PtrArray, i, &d->visibleMedia) {
        const iGmRun * run = i.ptr;
        if (run->mediaType == audio_GmRunMediaType) {
            iPlayerUI ui;
            init_PlayerUI(&ui,
                          audioPlayer_Media(media_GmDocument(d->doc), run->mediaId),
                          runRect_DocumentWidget_(d, run));
            draw_PlayerUI(&ui, p);
        }
        else if (run->mediaType == download_GmRunMediaType) {
            iDownloadUI ui;
            init_DownloadUI(&ui, d, run->mediaId, runRect_DocumentWidget_(d, run));
            draw_DownloadUI(&ui, p);
        }
    }
}

static void draw_DocumentWidget_(const iDocumentWidget *d) {
    const iWidget *w        = constAs_Widget(d);
    const iRect    bounds   = bounds_Widget(w);
    iVisBuf *      visBuf   = d->visBuf; /* will be updated now */
    if (width_Rect(bounds) <= 0) {
        return;
    }
    draw_Widget(w);
    allocVisBuffer_DocumentWidget_(d);
    const iRect ctxWidgetBounds = init_Rect(
        0, 0, width_Rect(bounds) - constAs_Widget(d->scroll)->rect.size.x, height_Rect(bounds));
    const iRect  docBounds = documentBounds_DocumentWidget_(d);
    iDrawContext ctx       = {
        .widget          = d,
        .showLinkNumbers = (d->flags & showLinkNumbers_DocumentWidgetFlag) != 0,
    };
    /* Currently visible region. */
    const iRangei vis  = visibleRange_DocumentWidget_(d);
    const iRangei full = { 0, size_GmDocument(d->doc).y };
    reposition_VisBuf(visBuf, vis);
    iRangei invalidRange[3];
    invalidRanges_VisBuf(visBuf, full, invalidRange);
    /* Redraw the invalid ranges. */ {
        iPaint *p = &ctx.paint;
        init_Paint(p);
        iForIndices(i, visBuf->buffers) {
            iVisBufTexture *buf = &visBuf->buffers[i];
            ctx.widgetBounds = moved_Rect(ctxWidgetBounds, init_I2(0, -buf->origin));
            ctx.viewPos      = init_I2(left_Rect(docBounds) - left_Rect(bounds), -buf->origin);
            if (!isEmpty_Rangei(invalidRange[i])) {
                beginTarget_Paint(p, buf->texture);
                if (isEmpty_Rangei(buf->validRange)) {
                    fillRect_Paint(p, (iRect){ zero_I2(), visBuf->texSize }, tmBackground_ColorId);
                }
                render_GmDocument(d->doc, invalidRange[i], drawRun_DrawContext_, &ctx);
            }
            /* Draw any invalidated runs that fall within this buffer. */ {
                const iRangei bufRange = { buf->origin, buf->origin + visBuf->texSize.y };
                /* Clear full-width backgrounds first in case there are any dynamic elements. */ {
                    iConstForEach(PtrSet, r, d->invalidRuns) {
                        const iGmRun *run = *r.value;
                        if (isOverlapping_Rangei(bufRange, ySpan_Rect(run->visBounds))) {
                            beginTarget_Paint(p, buf->texture);
                            fillRect_Paint(&ctx.paint,
                                           init_Rect(0,
                                                     run->visBounds.pos.y - buf->origin,
                                                     visBuf->texSize.x,
                                                     run->visBounds.size.y),
                                           tmBackground_ColorId);
                        }
                    }
                }
                iConstForEach(PtrSet, r, d->invalidRuns) {
                    const iGmRun *run = *r.value;
                    if (isOverlapping_Rangei(bufRange, ySpan_Rect(run->visBounds))) {
                        beginTarget_Paint(p, buf->texture);
                        drawRun_DrawContext_(&ctx, run);
                    }
                }
            }
            endTarget_Paint(&ctx.paint);
        }
        validate_VisBuf(visBuf);
        clear_PtrSet(d->invalidRuns);
    }
    setClip_Paint(&ctx.paint, bounds);
    const int yTop = docBounds.pos.y - value_Anim(&d->scrollY);
    draw_VisBuf(visBuf, init_I2(bounds.pos.x, yTop));
    /* Text markers. */
    if (!isEmpty_Range(&d->foundMark) || !isEmpty_Range(&d->selectMark)) {
        SDL_SetRenderDrawBlendMode(renderer_Window(get_Window()),
                                   isDark_ColorTheme(colorTheme_App()) ? SDL_BLENDMODE_ADD
                                                                       : SDL_BLENDMODE_BLEND);
        ctx.viewPos = topLeft_Rect(docBounds);
        /* Marker starting outside the visible range? */
        if (d->firstVisibleRun) {
            if (!isEmpty_Range(&d->selectMark) &&
                d->selectMark.start < d->firstVisibleRun->text.start &&
                d->selectMark.end > d->firstVisibleRun->text.start) {
                ctx.inSelectMark = iTrue;
            }
            if (isEmpty_Range(&d->foundMark) &&
                d->foundMark.start < d->firstVisibleRun->text.start &&
                d->foundMark.end > d->firstVisibleRun->text.start) {
                ctx.inFoundMark = iTrue;
            }
        }
        render_GmDocument(d->doc, vis, drawMark_DrawContext_, &ctx);
        SDL_SetRenderDrawBlendMode(renderer_Window(get_Window()), SDL_BLENDMODE_NONE);
    }
    drawMedia_DocumentWidget_(d, &ctx.paint);
    unsetClip_Paint(&ctx.paint);
    /* Fill the top and bottom, in case the document is short. */
    if (yTop > top_Rect(bounds)) {
        fillRect_Paint(&ctx.paint,
                       (iRect){ bounds.pos, init_I2(bounds.size.x, yTop - top_Rect(bounds)) },
                       hasSiteBanner_GmDocument(d->doc) ? tmBannerBackground_ColorId
                                                        : tmBackground_ColorId);
    }
    const int yBottom = yTop + size_GmDocument(d->doc).y;
    if (yBottom < bottom_Rect(bounds)) {
        fillRect_Paint(&ctx.paint,
                       init_Rect(bounds.pos.x, yBottom, bounds.size.x, bottom_Rect(bounds) - yBottom),
                       tmBackground_ColorId);
    }
    drawSideElements_DocumentWidget_(d);
    if (prefs_App()->hoverLink && d->hoverLink) {
        const int      font     = uiLabel_FontId;
        const iRangecc linkUrl  = range_String(linkUrl_GmDocument(d->doc, d->hoverLink->linkId));
        const iInt2    size     = measureRange_Text(font, linkUrl);
        const iRect    linkRect = { addY_I2(bottomLeft_Rect(bounds), -size.y),
                                    addX_I2(size, 2 * gap_UI) };
        fillRect_Paint(&ctx.paint, linkRect, tmBackground_ColorId);
        drawRange_Text(font, addX_I2(topLeft_Rect(linkRect), gap_UI), tmParagraph_ColorId, linkUrl);
    }
    if (colorTheme_App() == pureWhite_ColorTheme) {
        drawHLine_Paint(&ctx.paint, topLeft_Rect(bounds), width_Rect(bounds), uiSeparator_ColorId);
    }
    draw_Widget(w);
}

/*----------------------------------------------------------------------------------------------*/

iHistory *history_DocumentWidget(iDocumentWidget *d) {
    return d->mod.history;
}

const iString *url_DocumentWidget(const iDocumentWidget *d) {
    return d->mod.url;
}

const iGmDocument *document_DocumentWidget(const iDocumentWidget *d) {
    return d->doc;
}

const iBlock *sourceContent_DocumentWidget(const iDocumentWidget *d) {
    return &d->sourceContent;
}

int documentWidth_DocumentWidget(const iDocumentWidget *d) {
    return documentWidth_DocumentWidget_(d);
}

const iString *feedTitle_DocumentWidget(const iDocumentWidget *d) {
    if (!isEmpty_String(title_GmDocument(d->doc))) {
        return title_GmDocument(d->doc);
    }
    return bookmarkTitle_DocumentWidget(d);
}

const iString *bookmarkTitle_DocumentWidget(const iDocumentWidget *d) {
    iStringArray *title = iClob(new_StringArray());
    if (!isEmpty_String(title_GmDocument(d->doc))) {
        pushBack_StringArray(title, title_GmDocument(d->doc));
    }
    if (!isEmpty_String(d->titleUser)) {
        pushBack_StringArray(title, d->titleUser);
    }
    if (isEmpty_StringArray(title)) {
        iUrl parts;
        init_Url(&parts, d->mod.url);
        if (!isEmpty_Range(&parts.host)) {
            pushBackRange_StringArray(title, parts.host);
        }
    }
    if (isEmpty_StringArray(title)) {
        pushBackCStr_StringArray(title, "Blank Page");
    }
    return collect_String(joinCStr_StringArray(title, " \u2014 "));
}

void serializeState_DocumentWidget(const iDocumentWidget *d, iStream *outs) {
    serialize_PersistentDocumentState(&d->mod, outs);
}

void deserializeState_DocumentWidget(iDocumentWidget *d, iStream *ins) {
    deserialize_PersistentDocumentState(&d->mod, ins);
    parseUser_DocumentWidget_(d);
    updateFromHistory_DocumentWidget_(d);
}

void setUrlFromCache_DocumentWidget(iDocumentWidget *d, const iString *url, iBool isFromCache) {
    d->flags &= ~showLinkNumbers_DocumentWidgetFlag;
    set_String(d->mod.url, urlFragmentStripped_String(url));
    /* See if there a username in the URL. */
    parseUser_DocumentWidget_(d);
    if (!isFromCache || !updateFromHistory_DocumentWidget_(d)) {
        fetch_DocumentWidget_(d);
    }
}

iDocumentWidget *duplicate_DocumentWidget(const iDocumentWidget *orig) {
    iDocumentWidget *d = new_DocumentWidget();
    delete_History(d->mod.history);
    d->initNormScrollY = normScrollPos_DocumentWidget_(d);
    d->mod.history = copy_History(orig->mod.history);
    setUrlFromCache_DocumentWidget(d, orig->mod.url, iTrue);
    return d;
}

void setUrl_DocumentWidget(iDocumentWidget *d, const iString *url) {
    setUrlFromCache_DocumentWidget(d, url, iFalse);
}

void setInitialScroll_DocumentWidget(iDocumentWidget *d, float normScrollY) {
    d->initNormScrollY = normScrollY;
}

void setRedirectCount_DocumentWidget(iDocumentWidget *d, int count) {
    d->redirectCount = count;
}

iBool isRequestOngoing_DocumentWidget(const iDocumentWidget *d) {
    return d->request != NULL;
}

void updateSize_DocumentWidget(iDocumentWidget *d) {
    updateDocumentWidthRetainingScrollPosition_DocumentWidget_(d, iFalse);
    resetWideRuns_DocumentWidget_(d);
    updateSideIconBuf_DocumentWidget_(d);
    updateVisible_DocumentWidget_(d);
    invalidate_DocumentWidget_(d);
}

iBeginDefineSubclass(DocumentWidget, Widget)
    .processEvent = (iAny *) processEvent_DocumentWidget_,
    .draw         = (iAny *) draw_DocumentWidget_,
iEndDefineSubclass(DocumentWidget)

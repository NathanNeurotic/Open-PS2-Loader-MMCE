#include "include/opl.h"
#include "include/themes.h"
#include "include/util.h"
#include "include/gui.h"
#include "include/renderman.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/fntsys.h"
#include "include/lang.h"
#include "include/pad.h"
#include "include/sound.h"
#include "include/texcache.h"
#include "include/favsupport.h"
#include "include/vcdsupport.h" // vcdDisplayName -- display-only VCD game-ID prefix hide

#include <time.h>
#include <math.h>

#define MENU_POS_V               50
#define HINT_HEIGHT              32
#define DECORATOR_SIZE           20
#define APP_PREFETCH_IDLE_FRAMES 10

// Cache slots per AttributeImage element. An AttributeImage is NOT a per-game image -- it is a small FIXED
// SET of glyphs keyed by the attribute's value, so the cache only ever needs to hold that attribute's whole
// value set to be thrash-free. Our built-in attributes are tiny (#Format = ISO/ZSO/VCD/UL/ELF/HDL = 6,
// #DiscType = 3, #Media/#System = 2), but a theme can bind AttributeImage to a CUSTOM per-game config key
// (Genre, Developer, Publisher, ...) whose value set is open-ended (Gemini review of #49), so size for that:
// 32 covers any realistic custom attribute with headroom. Genuinely cheap -- cacheInitCache allocates only
// `count` empty cache_entry_t structs (no texture memory until a glyph is actually loaded into a slot), so
// 32 slots is ~2-3 KB of EE RAM and a #DiscType cache still only ever HOLDS its 3.
#define ATTR_IMAGE_CACHE_SLOTS 32

extern const char conf_theme_OPL_cfg;
extern u16 size_conf_theme_OPL_cfg;
extern const char theme_coverflow_cfg;
extern u16 size_theme_coverflow_cfg;

theme_t *gTheme;

// Set transiently around thmLoad(NULL) to load the embedded coverflow theme instead of
// the default OPL theme (the built-in "<Coverflow>" entry in the theme list).
static int gLoadCoverflowBuiltin = 0;

static int screenWidth;
static int screenHeight;
static int guiThemeID = 0;

static int nThemes = 0;
static theme_file_t themes[THM_MAX_FILES];
static const char **guiThemesNames = NULL;

// Coverflow render-mode state (externs in themes.h; defaults match wOPL 3/30/200/0).
#define COVERFLOW_PAD 1 // extra covers built off each edge, purely to fill the slide (see drawCoverFlow)
#define COVERFLOW_MAX (5 + 2 * COVERFLOW_PAD)
int gCoverflowCount = 3;        // 3 or 5 only (clamped on load AND at draw)
int gCoverflowCenterScale = 30; // px added to the center cover (UI 0/15/30/45)
int gCoverflowAnimSpeed = 200;  // ms (UI 0/100/200/400; 0 = instant, no anim)
int gCoverflowDimCovers = 0;    // bool

// Coverflow slide animation (cubic ease-out); armed by thmTriggerCoverflowAnim().
static int cfIsAnimating = 0;
static int cfAnimDirection = 0;
static clock_t cfAnimStartTime = 0;

enum ELEM_ATTRIBUTE_TYPE {
    ELEM_TYPE_ATTRIBUTE_TEXT = 0,
    ELEM_TYPE_STATIC_TEXT,
    ELEM_TYPE_ATTRIBUTE_IMAGE,
    ELEM_TYPE_GAME_IMAGE,
    ELEM_TYPE_STATIC_IMAGE,
    ELEM_TYPE_BACKGROUND, // A static image can be specified as the background. Otherwise, the plasma background will be drawn.
    ELEM_TYPE_MENU_ICON,
    ELEM_TYPE_MENU_TEXT,
    ELEM_TYPE_ITEMS_LIST,
    ELEM_TYPE_ITEM_ICON,
    ELEM_TYPE_ITEM_COVER,
    ELEM_TYPE_ITEM_TEXT,
    ELEM_TYPE_HINT_TEXT,
    ELEM_TYPE_INFO_HINT_TEXT,
    ELEM_TYPE_LOADING_ICON,
    ELEM_TYPE_BDM_INDEX,
    ELEM_TYPE_GAME_COUNT_TEXT,
    ELEM_TYPE_COVERFLOW,
    ELEM_TYPE_COUNT
};

#define DISPLAY_ALWAYS  0
#define DISPLAY_DEFINED 1
#define DISPLAY_NEVER   2

#define SIZING_NONE -1
#define SIZING_CLIP 0
#define SIZING_WRAP 1

static const char *elementsType[ELEM_TYPE_COUNT] = {
    "AttributeText",
    "StaticText",
    "AttributeImage",
    "GameImage",
    "StaticImage",
    "Background",
    "MenuIcon",
    "MenuText",
    "ItemsList",
    "ItemIcon",
    "ItemCover",
    "ItemText",
    "HintText",
    "InfoHintText",
    "LoadingIcon",
    "BdmIndex",
    "GameCountText",
    "Coverflow"};

// Per-device element filter (theme key devices=usb,hdd,... on MenuIcon/ItemsList/HintText) /////////////////////////////////
//
// Themer-facing device vocabulary, keyed to the SAME icon identity the MenuIcon renders
// (menu->item->icon_id): BDM pages re-resolve it per detected driver at mount time (opl.c's
// updateMenuFromGameList re-reads itemIconId), so a filter can never disagree with the visible
// icon, and one 'usb' filter naturally covers every USB slot. 'bdm' names the generic
// pre-mount/manual-start BDM page -- whose legacy TEXTURE name is literally "usb" (textures.c),
// which is why this table must never be derived from texture names. 'vcd' is deliberately
// absent: the L3 VCD view is an element FAMILY (vcdMain*), not a device; device filters keep
// working inside the vcd family, where icon_id remains the device's.
static const struct
{
    const char *name;
    int iconId;
} thmDeviceVocab[] = {
    {"usb", USB_ICON},
    {"ilink", ILINK_ICON},
    {"mx4sio", MX4SIO_ICON},
    {"hdd_bd", HDD_BD_ICON}, // internal exFAT (GPT/MBR) HDD via BDM
    {"hdd", HDD_ICON},       // internal APA/PFS HDD
    {"eth", ETH_ICON},
    {"smb", ETH_ICON}, // alias: same page/icon as eth
    {"mmce", MMCE_ICON},
    {"udpbd", UDP_ICON},
    {"udpfs", UDPFS_ICON},
    {"app", APP_ICON},
    {"fav", FAV_ICON},
    {"bdm", BDM_ICON}, // generic/unidentified BDM page (pre-mount or manual-start)
};
#define THM_DEVICE_VOCAB_COUNT ((int)(sizeof(thmDeviceVocab) / sizeof(thmDeviceVocab[0])))

// Parse a comma-separated devices= value into a vocab-index bitmask. Unknown names LOG and are
// ignored (quiet suppresses that -- addGUIElem pre-checks the same value initBasic then parses
// for real, and the warning should print once); an empty/unparseable value yields 0 = unfiltered,
// so a typo degrades to the pre-existing shared-element behavior instead of hiding the element
// everywhere. strtok_r, NOT strtok: theme loads run on the IO worker too (device NeedsUpdate ->
// thmAddElements) and could interleave with a GUI-thread strtok (e.g. neutrinoArgsParse).
static int thmParseDeviceList(const char *value, int quiet)
{
    int mask = 0;
    char *buf;
    char *tok;
    char *saveptr;

    if (value == NULL)
        return 0;

    buf = malloc(strlen(value) + 1);
    if (buf == NULL)
        return 0;
    strcpy(buf, value);

    for (tok = strtok_r(buf, ", \t", &saveptr); tok != NULL; tok = strtok_r(NULL, ", \t", &saveptr)) {
        int i, hit = 0;
        for (i = 0; i < THM_DEVICE_VOCAB_COUNT; i++) {
            if (strcasecmp(tok, thmDeviceVocab[i].name) == 0) {
                mask |= (1 << i);
                hit = 1;
                break;
            }
        }
        if (!hit && !quiet)
            LOG("THEMES devices=: unknown device name '%s' ignored\n", tok);
    }
    free(buf);
    return mask;
}

// Does a vocab-index bitmask cover the given page icon? Matching is by icon id, so aliases
// (smb/eth) and any future same-icon entries are free.
static int thmDeviceMaskMatches(int mask, int iconId)
{
    int i;
    if (mask == 0)
        return 0;
    for (i = 0; i < THM_DEVICE_VOCAB_COUNT; i++) {
        if ((mask & (1 << i)) && thmDeviceVocab[i].iconId == iconId)
            return 1;
    }
    return 0;
}

// Shared draw gate for MenuIcon/ItemsList/HintText: a FILTERED element draws only on its devices;
// an UNFILTERED element skips the devices a filtered same-type sibling covers (deviceCoverage,
// precomputed at theme load). Both fields zero -- every theme without the devices= key -- makes
// this a no-op, preserving byte-identical behavior.
static int thmElemSkipsDevice(const theme_element_t *elem, int iconId)
{
    if (elem->deviceFilter)
        return !thmDeviceMaskMatches(elem->deviceFilter, iconId);
    if (elem->deviceCoverage)
        return thmDeviceMaskMatches(elem->deviceCoverage, iconId);
    return 0;
}

// Nav-side twin of drawItemsList's gate: pick the FILTERED ItemsList that covers this page, else
// the family's slot element (fallback). menusys assigns gTheme->itemsList through this so paging
// math (displayedItems) always reads the exact element whose rows are on screen.
theme_element_t *thmResolveItemsList(theme_elems_t *family, theme_element_t *fallback, int iconId)
{
    theme_element_t *elem;

    if (family != NULL) {
        for (elem = family->first; elem != NULL; elem = elem->next) {
            if (elem->type == ELEM_TYPE_ITEMS_LIST && elem->deviceFilter && thmDeviceMaskMatches(elem->deviceFilter, iconId))
                return elem;
        }
    }
    return fallback;
}

// Precompute deviceCoverage per family: an UNFILTERED MenuIcon/ItemsList/HintText yields to
// filtered same-type siblings on the devices they cover (the themer's per-device override wins
// there; the unfiltered element stays the everywhere-else default). Runs once at theme load so the
// per-frame gate never walks the family. Families with no devices= keys leave everything 0.
static void thmComputeDeviceCoverage(theme_elems_t *elems)
{
    static const int filteredTypes[] = {ELEM_TYPE_MENU_ICON, ELEM_TYPE_ITEMS_LIST, ELEM_TYPE_HINT_TEXT};
    unsigned int t;

    for (t = 0; t < sizeof(filteredTypes) / sizeof(filteredTypes[0]); t++) {
        int covered = 0;
        theme_element_t *elem;

        for (elem = elems->first; elem != NULL; elem = elem->next) {
            if (elem->type == filteredTypes[t] && elem->deviceFilter)
                covered |= elem->deviceFilter;
        }
        if (!covered)
            continue;
        for (elem = elems->first; elem != NULL; elem = elem->next) {
            if (elem->type == filteredTypes[t] && !elem->deviceFilter)
                elem->deviceCoverage = covered;
        }
    }
}

// Common functions for Text ////////////////////////////////////////////////////////////////////////////////////////////////

static void endMutableText(theme_element_t *elem)
{
    mutable_text_t *mutableText = (mutable_text_t *)elem->extended;
    if (mutableText) {
        if (mutableText->value)
            free(mutableText->value);

        if (mutableText->alias)
            free(mutableText->alias);

        if (mutableText->wrappedValue)
            free(mutableText->wrappedValue);

        free(mutableText);
    }

    free(elem);
}

static mutable_text_t *initMutableText(const char *themePath, config_set_t *themeConfig, theme_t *theme, const char *name, int type, struct theme_element *elem, const char *value, const char *alias, int displayMode, int sizingMode)
{
    mutable_text_t *mutableText = (mutable_text_t *)malloc(sizeof(mutable_text_t));
    mutableText->currentConfigId = 0;
    mutableText->currentValue = NULL;
    mutableText->wrappedValue = NULL;
    mutableText->alias = NULL;

    char elemProp[64];

    snprintf(elemProp, sizeof(elemProp), "%s_display", name);
    configGetInt(themeConfig, elemProp, &displayMode);
    mutableText->displayMode = displayMode;

    int length = strlen(value) + 1;
    mutableText->value = (char *)malloc(length * sizeof(char));
    memcpy(mutableText->value, value, length);

    snprintf(elemProp, sizeof(elemProp), "%s_wrap", name);
    if (configGetInt(themeConfig, elemProp, &sizingMode)) {
        if (sizingMode > 0)
            sizingMode = SIZING_WRAP;
    }

    if ((elem->width != DIM_UNDEF) || (elem->height != DIM_UNDEF)) {
        if (sizingMode == SIZING_NONE)
            sizingMode = SIZING_CLIP;

        if (elem->width == DIM_UNDEF)
            elem->width = screenWidth;

        if (elem->height == DIM_UNDEF)
            elem->height = screenHeight;
    } else
        sizingMode = SIZING_NONE;
    mutableText->sizingMode = sizingMode;

    if (type == ELEM_TYPE_ATTRIBUTE_TEXT) {
        snprintf(elemProp, sizeof(elemProp), "%s_title", name);
        configGetStr(themeConfig, elemProp, &alias);
        if (!alias) {
            if (value[0] == '#')
                alias = &value[1];
            else
                alias = value;
        }

        char *temp;
        if (!strncmp(alias, "Title", 5))
            temp = _l(_STR_INFO_TITLE);
        else if (!strncmp(alias, "Genre", 5))
            temp = _l(_STR_INFO_GENRE);
        else if (!strncmp(alias, "Release", 7))
            temp = _l(_STR_INFO_RELEASE);
        else if (!strncmp(alias, "Developer", 9))
            temp = _l(_STR_INFO_DEVELOPER);
        else if (!strncmp(alias, "Size", 4))
            temp = _l(_STR_SIZE);
        else if (!strncmp(alias, "Description", 11))
            temp = _l(_STR_INFO_DESCRIPTION);
        else
            temp = (char *)alias;

        length = strlen(temp) + 1 + 2;
        mutableText->alias = (char *)calloc(length, sizeof(char));
        if (mutableText->sizingMode == SIZING_WRAP)
            snprintf(mutableText->alias, length, "%s:\n", temp);
        else
            snprintf(mutableText->alias, length, "%s: ", temp);
    } else {
        if (mutableText->sizingMode == SIZING_WRAP)
            fntFitString(elem->font, mutableText->value, elem->width);
    }

    return mutableText;
}

// StaticText ///////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void drawStaticText(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    mutable_text_t *mutableText = (mutable_text_t *)elem->extended;
    if (mutableText->sizingMode == SIZING_NONE)
        fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, 0, 0, mutableText->value, elem->color);
    else
        fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, mutableText->value, elem->color);
}

static void initStaticText(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name)
{
    const char *value = NULL; // configGetStr leaves this untouched if the key is absent
    char elemProp[64];

    snprintf(elemProp, sizeof(elemProp), "%s_value", name);
    configGetStr(themeConfig, elemProp, &value);
    if (value) {
        elem->extended = initMutableText(themePath, themeConfig, theme, name, ELEM_TYPE_STATIC_TEXT, elem, value, NULL, DISPLAY_ALWAYS, SIZING_NONE);
        elem->endElem = &endMutableText;
        elem->drawElem = &drawStaticText;
    } else
        LOG("THEMES StaticText %s: NO value, elem disabled !!\n", name);
}

// GameCountText ////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int getGameCount(void *support)
{
    item_list_t *list = (item_list_t *)support;
    return list->itemGetCount(list);
}

static void drawGameCountText(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    mutable_text_t *mutableText = (mutable_text_t *)elem->extended;

    if (config) {
        if (mutableText->currentConfigId != config->uid) {
            // force refresh
            mutableText->currentConfigId = config->uid;

            int count = getGameCount(menu->item->userdata);
            snprintf(mutableText->value, sizeof(char) * 60, _l(_STR_FILE_COUNT), count);
        }
    }

    fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, 0, 0, mutableText->value, elem->color);
}

static void initGameCountText(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name)
{
    /* drawGameCountText() snprintf()s up to 60 bytes into mutableText->value.
     * initMutableText() sizes value from strlen(seed)+1, so seeding with an
     * empty string previously produced a 1-byte buffer that the 60-byte
     * snprintf overran (and the separate 60-byte countStr was leaked).  Seed
     * with "" then replace value with a correctly sized 60-byte buffer. */
    mutable_text_t *mutableText = initMutableText(themePath, themeConfig, theme, name, ELEM_TYPE_ATTRIBUTE_TEXT, elem, "", NULL, DISPLAY_ALWAYS, SIZING_NONE);
    elem->extended = mutableText;

    if (mutableText != NULL) {
        char *countStr = (char *)malloc(60);
        if (countStr != NULL) {
            countStr[0] = '\0';
            free(mutableText->value);
            mutableText->value = countStr;
        }
    }

    elem->endElem = &endMutableText;
    elem->drawElem = &drawGameCountText;
}

// AttributeText ////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void drawAttributeText(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    // No current item (e.g. the last favourite was removed -> empty list): clear, don't redraw the
    // stale cached value. Mirrors drawGameImage's `if (item)` guard so the description clears (#48).
    if (item == NULL)
        return;
    mutable_text_t *mutableText = (mutable_text_t *)elem->extended;
    if (config) {
        if (mutableText->currentConfigId != config->uid) {
            // force refresh
            mutableText->currentConfigId = config->uid;
            mutableText->currentValue = NULL;
            if (configGetStr(config, mutableText->value, (const char **)&mutableText->currentValue)) {
                if ((mutableText->sizingMode == SIZING_WRAP) && mutableText->currentValue) {
                    // Word-wrap a private copy, not the config's own stored string.
                    // configGetStr() hands back a pointer straight into the config
                    // buffer (it->val); fntFitString() inserts '\n's in place, so
                    // wrapping currentValue directly would bake the line breaks into
                    // the stored value. Those then get written back to the .cfg on the
                    // next settings save, corrupting the field (issue #44). Copying
                    // first keeps the on-screen wrapping while leaving the value intact.
                    free(mutableText->wrappedValue);
                    mutableText->wrappedValue = NULL;
                    size_t len = strlen(mutableText->currentValue) + 1;
                    char *wrapped = (char *)malloc(len);
                    if (wrapped) {
                        memcpy(wrapped, mutableText->currentValue, len);
                        fntFitString(elem->font, wrapped, elem->width);
                        mutableText->wrappedValue = wrapped;
                        mutableText->currentValue = wrapped;
                    }
                }
            }
        }
        if (mutableText->currentValue) {
            char result[300];
            if (mutableText->displayMode == DISPLAY_NEVER) {
                if (!strncmp(mutableText->alias, _l(_STR_SIZE), strlen(_l(_STR_SIZE)))) {
                    snprintf(result, sizeof(result), "%s MiB", mutableText->currentValue);
                    if (mutableText->sizingMode == SIZING_NONE)
                        fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, 0, 0, result, elem->color);
                    else
                        fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, result, elem->color);
                } else {
                    if (mutableText->sizingMode == SIZING_NONE)
                        fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, 0, 0, mutableText->currentValue, elem->color);
                    else
                        fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, mutableText->currentValue, elem->color);
                }
            } else {
                if (!strncmp(mutableText->alias, _l(_STR_SIZE), strlen(_l(_STR_SIZE))))
                    snprintf(result, sizeof(result), "%s%s MiB", mutableText->alias, mutableText->currentValue);
                else
                    snprintf(result, sizeof(result), "%s%s", mutableText->alias, mutableText->currentValue);
                if (mutableText->sizingMode == SIZING_NONE)
                    fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, 0, 0, result, elem->color);
                else
                    fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, result, elem->color);
            }
            return;
        }
    }
    if (mutableText->displayMode == DISPLAY_ALWAYS) {
        if (mutableText->sizingMode == SIZING_NONE)
            fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, 0, 0, mutableText->alias, elem->color);
        else
            fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, mutableText->alias, elem->color);
    }
}

static void initAttributeText(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name)
{
    const char *attribute = NULL; // configGetStr leaves this untouched if the key is absent
    char elemProp[64];

    snprintf(elemProp, sizeof(elemProp), "%s_attribute", name);
    configGetStr(themeConfig, elemProp, &attribute);
    if (attribute) {
        elem->extended = initMutableText(themePath, themeConfig, theme, name, ELEM_TYPE_ATTRIBUTE_TEXT, elem, attribute, NULL, DISPLAY_ALWAYS, SIZING_NONE);
        elem->endElem = &endMutableText;
        elem->drawElem = &drawAttributeText;
    } else
        LOG("THEMES AttributeText %s: NO attribute, elem disabled !!\n", name);
}

// Common functions for Image ///////////////////////////////////////////////////////////////////////////////////////////////

static void findDuplicate(theme_element_t *first, const char *cachePattern, const char *defaultTexture, const char *overlayTexture, const char *overlayTexture2, mutable_image_t *target)
{
    theme_element_t *elem = first;
    while (elem) {
        if ((elem->type == ELEM_TYPE_STATIC_IMAGE) || (elem->type == ELEM_TYPE_ATTRIBUTE_IMAGE) || (elem->type == ELEM_TYPE_GAME_IMAGE) || (elem->type == ELEM_TYPE_BACKGROUND)) {
            mutable_image_t *source = (mutable_image_t *)elem->extended;

            if (cachePattern && source->cache && !strcmp(cachePattern, source->cache->suffix)) {
                target->cache = source->cache;
                target->cacheLinked = 1;
                LOG("THEMES Re-using a cache for pattern %s\n", cachePattern);
            }

            if (defaultTexture && source->defaultTexture && !strcmp(defaultTexture, source->defaultTexture->name)) {
                target->defaultTexture = source->defaultTexture;
                target->defaultTextureLinked = 1;
                LOG("THEMES Re-using the default texture for %s\n", defaultTexture);
            }

            // The overlay (cover-window) texture is intentionally NOT shared by name here. Its
            // per-element cover-window corners (overlay_ulx..lry) are stored ON the image_texture_t,
            // so reusing the object would make a later element inherit an earlier one's corners --
            // e.g. the square apps/VCD coverflow (own corners 0..184/0..256-square) inheriting the
            // games' 184x256 window, which makes the cover IMAGE draw past its frame at the other
            // page's rectangle size. Each element loads its own overlay (initImageTexture, called
            // below in initMutableImage) so its own corners are read. The case bitmap is tiny +
            // paletted; the expensive case_overlay glare (overlay2, no corners, plain pixmap) is
            // still safely shared just below.
            (void)overlayTexture;

            if (overlayTexture2 && source->overlayTexture2 && !strcmp(overlayTexture2, source->overlayTexture2->name)) {
                target->overlayTexture2 = source->overlayTexture2;
                target->overlayTexture2Linked = 1;
                LOG("THEMES Re-using the overlay2 texture for %s\n", overlayTexture2);
            }
        }

        elem = elem->next;
    }
}

static void freeImageTexture(image_texture_t *texture)
{
    if (texture) {
        if (texture->source.Mem) {
            rmUnloadTexture(&texture->source);
            free(texture->source.Mem);
            texture->source.Mem = NULL;
        }
        if (texture->source.Clut) {
            free(texture->source.Clut);
            texture->source.Clut = NULL;
        }
        if (texture->name) {
            free(texture->name);
            texture->name = NULL;
        }
        free(texture);
    }
}

static image_texture_t *initImageTexture(const char *themePath, config_set_t *themeConfig, const char *name, const char *imgName, int isOverlay)
{
    // calloc so omitted overlay coordinate keys default to 0 instead of
    // uninitialized heap garbage that would feed rmDrawOverlayPixmap.
    image_texture_t *texture = (image_texture_t *)calloc(1, sizeof(image_texture_t));
    texture->name = NULL;

    int texId = -1;
    int result = 0;

    // Propagate the actual load result so a missing/corrupt texture takes the
    // free-and-return-NULL path below instead of becoming a live element whose
    // source.Mem is NULL (the empty `if (...) ;` previously discarded it).
    if (themePath) {
        char path[256];
        snprintf(path, sizeof(path), "%s%s", themePath, imgName);
        result = (texDiscoverLoad(&texture->source, path, texId) >= 0);
    } else {
        texId = texLookupInternalTexId(imgName);
        result = (texLoadInternal(&texture->source, texId) >= 0);
    }

    if (result) {
        int length = strlen(imgName) + 1;
        texture->name = (char *)malloc(length * sizeof(char));
        memcpy(texture->name, imgName, length);

        if (isOverlay) {
            int intValue;
            char elemProp[64];
            snprintf(elemProp, sizeof(elemProp), "%s_overlay_ulx", name);
            if (configGetInt(themeConfig, elemProp, &intValue))
                texture->upperLeft_x = intValue;
            snprintf(elemProp, sizeof(elemProp), "%s_overlay_uly", name);
            if (configGetInt(themeConfig, elemProp, &intValue))
                texture->upperLeft_y = intValue;
            snprintf(elemProp, sizeof(elemProp), "%s_overlay_urx", name);
            if (configGetInt(themeConfig, elemProp, &intValue))
                texture->upperRight_x = intValue;
            snprintf(elemProp, sizeof(elemProp), "%s_overlay_ury", name);
            if (configGetInt(themeConfig, elemProp, &intValue))
                texture->upperRight_y = intValue;
            snprintf(elemProp, sizeof(elemProp), "%s_overlay_llx", name);
            if (configGetInt(themeConfig, elemProp, &intValue))
                texture->lowerLeft_x = intValue;
            snprintf(elemProp, sizeof(elemProp), "%s_overlay_lly", name);
            if (configGetInt(themeConfig, elemProp, &intValue))
                texture->lowerLeft_y = intValue;
            snprintf(elemProp, sizeof(elemProp), "%s_overlay_lrx", name);
            if (configGetInt(themeConfig, elemProp, &intValue))
                texture->lowerRight_x = intValue;
            snprintf(elemProp, sizeof(elemProp), "%s_overlay_lry", name);
            if (configGetInt(themeConfig, elemProp, &intValue))
                texture->lowerRight_y = intValue;
        }
    } else {
        freeImageTexture(texture);
        texture = NULL;
    }

    return texture;
}

static image_texture_t *initImageInternalTexture(config_set_t *themeConfig, const char *name)
{
    // calloc so the embedded source GSTEXTURE is zeroed: if texLookupInternalTexId
    // fails below, freeImageTexture() reads texture->source.Mem/.Clut and must not
    // see uninitialized garbage (which it would rmUnloadTexture()/free()).
    image_texture_t *texture = (image_texture_t *)calloc(1, sizeof(image_texture_t));
    texture->name = NULL;
    int result;

    if ((result = texLookupInternalTexId(name)) >= 0) {
        result = texLoadInternal(&texture->source, result);
        int length = strlen(name) + 1;
        texture->name = (char *)malloc(length * sizeof(char));
        memcpy(texture->name, name, length);
    }

    if (result < 0) {
        freeImageTexture(texture);
        texture = NULL;
    }

    return texture;
}

static void endMutableImage(struct theme_element *elem)
{
    mutable_image_t *mutableImage = (mutable_image_t *)elem->extended;
    if (mutableImage) {
        if (mutableImage->cache && !mutableImage->cacheLinked)
            cacheDestroyCache(mutableImage->cache);

        if (mutableImage->defaultTexture && !mutableImage->defaultTextureLinked)
            freeImageTexture(mutableImage->defaultTexture);

        if (mutableImage->overlayTexture && !mutableImage->overlayTextureLinked)
            freeImageTexture(mutableImage->overlayTexture);

        if (mutableImage->overlayTexture2 && !mutableImage->overlayTexture2Linked)
            freeImageTexture(mutableImage->overlayTexture2);

        free(mutableImage);
    }

    free(elem);
}

static mutable_image_t *initMutableImage(const char *themePath, config_set_t *themeConfig, theme_t *theme, const char *name, int type, const char *cachePattern, int cacheCount, const char *defaultTexture, const char *overlayTexture)
{
    mutable_image_t *mutableImage = (mutable_image_t *)malloc(sizeof(mutable_image_t));
    mutableImage->currentCacheId = -1; // -1 = the cache API's "no entry" sentinel (see themes.h)
    mutableImage->currentUid = -1;
    mutableImage->currentConfigId = 0;
    mutableImage->currentValue = NULL;
    mutableImage->cache = NULL;
    mutableImage->cacheLinked = 0;
    mutableImage->defaultTexture = NULL;
    mutableImage->defaultTextureLinked = 0;
    mutableImage->overlayTexture = NULL;
    mutableImage->overlayTextureLinked = 0;
    mutableImage->overlayTexture2 = NULL;
    mutableImage->overlayTexture2Linked = 0;

    char elemProp[64];

    if (type == ELEM_TYPE_ATTRIBUTE_IMAGE) {
        snprintf(elemProp, sizeof(elemProp), "%s_attribute", name);
        configGetStr(themeConfig, elemProp, &cachePattern);
        LOG("THEMES MutableImage %s: type: %s using cache pattern: %s\n", name, elementsType[type], cachePattern);
    } else if ((type == ELEM_TYPE_GAME_IMAGE) || (type == ELEM_TYPE_BACKGROUND)) {
        snprintf(elemProp, sizeof(elemProp), "%s_pattern", name);
        configGetStr(themeConfig, elemProp, &cachePattern);
        snprintf(elemProp, sizeof(elemProp), "%s_count", name);
        configGetInt(themeConfig, elemProp, &cacheCount);
        if (cachePattern != NULL && strcmp(cachePattern, "COV") == 0 && cacheCount < 2)
            cacheCount = 2;
        LOG("THEMES MutableImage %s: type: %s using cache pattern: %s count: %d\n", name, elementsType[type], cachePattern, cacheCount);
    }

    snprintf(elemProp, sizeof(elemProp), "%s_default", name);
    configGetStr(themeConfig, elemProp, &defaultTexture);

    if (type != ELEM_TYPE_BACKGROUND) {
        snprintf(elemProp, sizeof(elemProp), "%s_overlay", name);
        configGetStr(themeConfig, elemProp, &overlayTexture);
    }

    const char *overlayTexture2 = NULL;
    if (type != ELEM_TYPE_BACKGROUND) {
        snprintf(elemProp, sizeof(elemProp), "%s_overlay2", name);
        configGetStr(themeConfig, elemProp, &overlayTexture2);
    }

    findDuplicate(theme->mainElems.first, cachePattern, defaultTexture, overlayTexture, overlayTexture2, mutableImage);
    findDuplicate(theme->infoElems.first, cachePattern, defaultTexture, overlayTexture, overlayTexture2, mutableImage);
    findDuplicate(theme->appsMainElems.first, cachePattern, defaultTexture, overlayTexture, overlayTexture2, mutableImage);
    findDuplicate(theme->appsInfoElems.first, cachePattern, defaultTexture, overlayTexture, overlayTexture2, mutableImage);
    findDuplicate(theme->favsMainElems.first, cachePattern, defaultTexture, overlayTexture, overlayTexture2, mutableImage);
    findDuplicate(theme->favsInfoElems.first, cachePattern, defaultTexture, overlayTexture, overlayTexture2, mutableImage);
    findDuplicate(theme->vcdMainElems.first, cachePattern, defaultTexture, overlayTexture, overlayTexture2, mutableImage);
    findDuplicate(theme->vcdInfoElems.first, cachePattern, defaultTexture, overlayTexture, overlayTexture2, mutableImage);

    if (cachePattern && !mutableImage->cache) {
        if (type == ELEM_TYPE_ATTRIBUTE_IMAGE)
            // An AttributeImage is a small FIXED SET of glyphs keyed by the attribute's value, not a
            // per-game image: #DiscType has three (PS1CD/PS2CD/PS2DVD), #Media two (CD/DVD), #System two
            // (PS1/PS2), #Format a handful. With ONE slot the cache could hold exactly one of them, so
            // every move between a DVD game and a CD game EVICTED it and re-read the PNG off the device --
            // and that re-read rides the single art worker, queued BEHIND the whole interactive art set
            // (the cover shows in ~0.2s but the rest drains for seconds). That is AcidReach's #49 report
            // exactly: ~5s on a slow step to a new game, ~0.5s back to a previous one, and -- the tell --
            // ~0.2s when scrolling FAST, because scrolling defers the art set and the badge jumps a clear
            // queue. ATTR_IMAGE_CACHE_SLOTS covers the largest attribute's value set, so each glyph is read
            // ONCE per session and every later selection is a RAM hit with zero device IO.
            mutableImage->cache = cacheInitCache(-1, themePath, 0, cachePattern, ATTR_IMAGE_CACHE_SLOTS);
        else
            mutableImage->cache = cacheInitCache(theme->gameCacheCount++, "ART", 1, cachePattern, cacheCount);
    }

    if (!themePath)
        if (defaultTexture && !mutableImage->defaultTexture)
            mutableImage->defaultTexture = initImageInternalTexture(themeConfig, defaultTexture);

    if (defaultTexture && !mutableImage->defaultTexture)
        mutableImage->defaultTexture = initImageTexture(themePath, themeConfig, name, defaultTexture, 0);

    if (overlayTexture && !mutableImage->overlayTexture)
        mutableImage->overlayTexture = initImageTexture(themePath, themeConfig, name, overlayTexture, 1);

    // overlay2 is drawn over the cover composite (plain rmDrawPixmap), so it needs no cover-window
    // corners -- load it as a plain texture (isOverlay=0).
    if (overlayTexture2 && !mutableImage->overlayTexture2)
        mutableImage->overlayTexture2 = initImageTexture(themePath, themeConfig, name, overlayTexture2, 0);

    return mutableImage;
}

// StaticImage //////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void drawStaticImage(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    mutable_image_t *staticImage = (mutable_image_t *)elem->extended;
    if (staticImage->overlayTexture) {
        rmDrawOverlayPixmap(&staticImage->overlayTexture->source, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol,
                            &staticImage->defaultTexture->source, staticImage->overlayTexture->upperLeft_x, staticImage->overlayTexture->upperLeft_y, staticImage->overlayTexture->upperRight_x, staticImage->overlayTexture->upperRight_y,
                            staticImage->overlayTexture->lowerLeft_x, staticImage->overlayTexture->lowerLeft_y, staticImage->overlayTexture->lowerRight_x, staticImage->overlayTexture->lowerRight_y, 0);
        if (staticImage->overlayTexture2)
            rmDrawPixmap(&staticImage->overlayTexture2->source, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol, 0);
    } else
        rmDrawPixmap(&staticImage->defaultTexture->source, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol, 0);
}

static void initStaticImage(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name, const char *imageName)
{
    mutable_image_t *mutableImage = initMutableImage(themePath, themeConfig, theme, name, elem->type, NULL, 0, imageName, NULL);
    elem->extended = mutableImage;
    elem->endElem = &endMutableImage;

    if (mutableImage->defaultTexture)
        elem->drawElem = &drawStaticImage;
    else
        LOG("THEMES StaticImage %s: NO image name, elem disabled !!\n", name);
}

// GameImage ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static GSTEXTURE *getGameImageTexture(image_cache_t *cache, void *support, struct submenu_item *item)
{
    // Folder browsing: a folder row has no cover art (and no startup key). Never route it through the
    // cover cache -- an empty key would thrash the cache and paint the empty case frame. Applies to
    // every consumer: the list decorator, the big cover panel and the coverflow carousel.
    if (item != NULL && item->isFolder)
        return NULL;

    if (gEnableArt) {
        item_list_t *list = (item_list_t *)support;
        char *startup = list->itemGetStartup(list, item->id);
        return cacheGetTexture(cache, list, &item->cache_id[cache->userId], &item->cache_uid[cache->userId], startup);
    }

    return NULL;
}

static int canPrefetchAdjacentGameImages(image_cache_t *cache, item_list_t *list, GSTEXTURE *selectedTexture)
{
    if (cache == NULL || list == NULL || selectedTexture == NULL || selectedTexture->Mem == NULL)
        return 0;

    if (list->mode == MMCE_MODE)
        return 0;

    if (list->mode == APP_MODE) {
        if (guiInactiveFrames < APP_PREFETCH_IDLE_FRAMES || cacheHasPendingInteractiveArt())
            return 0;
    }

    return 1;
}

static void prefetchGameImageTexture(image_cache_t *cache, void *support, struct submenu_list *item, int minInactiveFrames)
{
    item_list_t *list;
    char *startup;

    if (cache == NULL || item == NULL || guiInactiveFrames < minInactiveFrames)
        return;

    list = (item_list_t *)support;
    if (list == NULL)
        return;

    startup = list->itemGetStartup(list, item->item.id);
    cachePrefetchTexture(cache, list, &item->item.cache_id[cache->userId], &item->item.cache_uid[cache->userId], startup);
}

static void prefetchAdjacentGameImages(image_cache_t *cache, void *support, struct submenu_list *item, int distance, int minInactiveFrames)
{
    struct submenu_list *nextItem = item;
    struct submenu_list *prevItem = item;

    if (item == NULL || distance <= 0)
        return;

    for (int i = 0; i < distance; i++) {
        if (nextItem != NULL)
            nextItem = nextItem->next;
        if (prevItem != NULL)
            prevItem = prevItem->prev;

        prefetchGameImageTexture(cache, support, nextItem, minInactiveFrames);
        prefetchGameImageTexture(cache, support, prevItem, minInactiveFrames);
    }
}

// Favourites element redirection (defined in the Coverflow section below; used by both draw
// paths so an APP favourite renders with the apps element, not the game cover element).
static theme_element_t *thmGetElemForItem(struct menu_list *menu, struct submenu_list *item, theme_element_t *elem);

static void drawGameImage(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    if (item) {
        item_list_t *list = (item_list_t *)menu->item->userdata;

        // On the Favourites tab an APP favourite draws with the theme's apps element (its own
        // dimensions, case overlay and art folder); games and every other tab use elem as-is.
        // The texture lookup stays on menu->item->userdata (favList) so favGetImage proxies by
        // the FAV index -- only the element (geometry + cache + overlay) is redirected.
        struct theme_element *drawElem = thmGetElemForItem(menu, item, elem);
        mutable_image_t *gameImage = (mutable_image_t *)drawElem->extended;
        if (gameImage == NULL)
            return;

        GSTEXTURE *texture = getGameImageTexture(gameImage->cache, menu->item->userdata, &item->item);

        if (gameImage->cache != NULL && gameImage->cache->suffix != NULL && strcmp(gameImage->cache->suffix, "COV") == 0 &&
            canPrefetchAdjacentGameImages(gameImage->cache, list, texture)) {
            int prefetchInactiveFrames = (list != NULL && list->mode == APP_MODE) ? APP_PREFETCH_IDLE_FRAMES : MENU_MIN_INACTIVE_FRAMES;
            prefetchAdjacentGameImages(gameImage->cache, menu->item->userdata, item, 1, prefetchInactiveFrames);
        }

        if (!texture || !texture->Mem) {
            // #2: on the Favourites page a COVER element with no real art must not draw the embedded
            // placeholder wrapped in the case frame (the hollow grey "empty tray" box). Suppress the
            // COVER only -- keyed on the cache suffix AND excluding Background elements (a theme may
            // bind a COV pattern to its Background; that must keep its defaultTexture/plasma fallback)
            // -- so info-page screenshots (SCR/SCR2) and backgrounds keep their placeholders. Only
            // while Cover Art is ON: with gEnableArt off every favourite reads as "no art" and the
            // suppression would blank the whole tab (Games/Apps show placeholders there; match them).
            int isCover = gameImage->cache != NULL && gameImage->cache->suffix != NULL && strcmp(gameImage->cache->suffix, "COV") == 0;
            if (gEnableArt && isCover && elem->type != ELEM_TYPE_BACKGROUND && list != NULL && list->mode == FAV_MODE)
                return; // no real art -> draw nothing (no empty case frame) for a Favourites cover
            if (gameImage->defaultTexture)
                texture = &gameImage->defaultTexture->source;
            else {
                if (elem->type == ELEM_TYPE_BACKGROUND)
                    guiDrawBGPlasma();
                return;
            }
        }

        if (gameImage->overlayTexture) {
            rmDrawOverlayPixmap(&gameImage->overlayTexture->source, drawElem->posX, drawElem->posY, drawElem->aligned, drawElem->width, drawElem->height, drawElem->scaled, gDefaultCol,
                                texture, gameImage->overlayTexture->upperLeft_x, gameImage->overlayTexture->upperLeft_y, gameImage->overlayTexture->upperRight_x, gameImage->overlayTexture->upperRight_y,
                                gameImage->overlayTexture->lowerLeft_x, gameImage->overlayTexture->lowerLeft_y, gameImage->overlayTexture->lowerRight_x, gameImage->overlayTexture->lowerRight_y, 0);
            if (gameImage->overlayTexture2)
                rmDrawPixmap(&gameImage->overlayTexture2->source, drawElem->posX, drawElem->posY, drawElem->aligned, drawElem->width, drawElem->height, drawElem->scaled, gDefaultCol, 0);
        } else
            rmDrawPixmap(texture, drawElem->posX, drawElem->posY, drawElem->aligned, drawElem->width, drawElem->height, drawElem->scaled, gDefaultCol, 0);

    } else if (elem->type == ELEM_TYPE_BACKGROUND) {
        mutable_image_t *gameImage = (mutable_image_t *)elem->extended;
        if (gameImage->defaultTexture)
            rmDrawPixmap(&gameImage->defaultTexture->source, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol, 0);
        else
            guiDrawBGPlasma();
    }
}

static void initGameImage(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name, const char *pattern, int count, const char *texture, const char *overlay)
{
    mutable_image_t *mutableImage = initMutableImage(themePath, themeConfig, theme, name, elem->type, pattern, count, texture, overlay);
    elem->extended = mutableImage;
    elem->endElem = &endMutableImage;

    if (mutableImage->cache)
        elem->drawElem = &drawGameImage;
    else
        LOG("THEMES GameImage %s: NO pattern, elem disabled !!\n", name);
}

// Coverflow ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Source list used for a submenu item's TEXTURE lookup. Always the active menu's userdata --
// including the Favourites tab: our FAV items carry a FAV-array index (not the source id), and
// favGetImage / favGetItemStartup proxy by that index, so the lookup must stay on favList.
static void *thmGetItemSource(struct menu_list *menu, struct submenu_list *item)
{
    return menu->item->userdata;
}

// Find a game-image / coverflow element in a list by its cover-cache suffix (e.g. "COV").
static theme_element_t *thmFindElemBySuffix(theme_elems_t *elems, const char *suffix)
{
    if (suffix == NULL)
        return NULL;
    for (theme_element_t *e = elems->first; e != NULL; e = e->next) {
        if (e->type != ELEM_TYPE_GAME_IMAGE && e->type != ELEM_TYPE_COVERFLOW)
            continue;
        mutable_image_t *eimg = (mutable_image_t *)e->extended;
        if (eimg != NULL && eimg->cache != NULL && eimg->cache->suffix != NULL && strcmp(eimg->cache->suffix, suffix) == 0)
            return e;
    }
    return NULL;
}

// Element to DRAW a submenu item with. The Favourites screen as a whole renders with the theme's
// favs family (menuRenderMain switches the element set per mode); the VCD view and the Apps tab
// render with the apps family the same way. This per-item hook adds ONE extra redirect on top: an
// APP-source favourite redirects its COVER to the apps element (matched by cache suffix) so a
// favourited app keeps its app box even when the theme defines no distinct favsMain cover. Non-app
// favourites and every other screen pass through unchanged -- the element already comes from the
// correct family. Single chokepoint for both drawGameImage and the coverflow carousel.
static theme_element_t *thmGetElemForItem(struct menu_list *menu, struct submenu_list *item, theme_element_t *elem)
{
    if (item == NULL || elem == NULL)
        return elem;
    item_list_t *menuList = (item_list_t *)menu->item->userdata;
    if (menuList == NULL || menuList->mode != FAV_MODE)
        return elem;
    if (favGetItemSourceMode(item->item.id) != APP_MODE)
        return elem;
    mutable_image_t *img = (mutable_image_t *)elem->extended;
    if (img == NULL || img->cache == NULL)
        return elem;
    theme_element_t *appsElem = thmFindElemBySuffix(&gTheme->appsMainElems, img->cache->suffix);
    return (appsElem != NULL) ? appsElem : elem;
}

// Arms a slide animation in direction dir (+1 next / -1 prev). No-op (instant move) when
// animation speed is 0. Called from menusys.c via the themes.h extern.
void thmTriggerCoverflowAnim(int dir)
{
    if (gCoverflowAnimSpeed <= 0)
        return;
    cfIsAnimating = 1;
    cfAnimDirection = dir;
    cfAnimStartTime = clock();
}

static void drawCoverFlow(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    if (!item)
        return; // empty list -> nothing to draw

    if (elem->extended == NULL)
        return; // coverflow element disabled (no cover cache)
    item_list_t *sourceList = (item_list_t *)thmGetItemSource(menu, item);

    // Defensive clamp: never trust the global at the draw site (conf.cfg may be hand-edited
    // to e.g. 7 -> covers[] OOB). coverCount is the VISIBLE count and drives layout only;
    // coverTotal (= coverCount + 2*COVERFLOW_PAD) is the index bound for covers[].
    int coverCount = (gCoverflowCount == 5) ? 5 : 3;
    int centerIndex = coverCount / 2;

    // Layout in virtual 640x480 space (div-guarded). SCALING_RATIO + the panel apply
    // the widescreen/PAR correction at draw time (rmSetupQuad: w * iAspectWidth >> 2),
    // exactly like the stock ItemCover path. We must NOT pre-apply rmWideScale here or
    // the cover gets the aspect factor twice and warps when widescreen is toggled.
    int coverWidth = elem->width;
    int maxCoverWidth = (screenWidth - (coverCount - 1) * 10) / coverCount;
    // Widescreen draws each cover at 3/4 width (SCALING_RATIO), leaving spare screen, so
    // allow ~10% larger covers in 16:9. This scales SIZE only -- the cover's authored W:H
    // is untouched, so aspect immunity holds; 4:3 keeps the tight 3-up maximum.
    if (gWideScreen)
        maxCoverWidth = maxCoverWidth * 11 / 10;
    if (coverWidth > maxCoverWidth)
        coverWidth = maxCoverWidth;
    if (coverWidth <= 0)
        return;

    int coverSpacing = (screenWidth - coverCount * coverWidth) / (coverCount + 1);
    if (coverSpacing < 0)
        coverSpacing = 0;
    int coverDistance = coverWidth + coverSpacing;
    int basePosX = (screenWidth - (coverCount * coverWidth + (coverCount - 1) * coverSpacing)) / 2 + coverWidth / 2 + coverWidth * gTheme->coverflowCoverOffset / 256;

    // Carousel cover SIZE/aspect comes from the coverflow element (uniform across covers). The
    // per-cover frame-inset recenter is computed inside the loop from each cover's OWN element +
    // overlay -- the Favourites tab can mix game and app cases (different frame insets), so a
    // single shared recenter would shift the odd one out.

    /*
      Build the window: covers[centerIdx] is the selection; fan out both sides, wrapping
      last<->first. The left wrap reads menu->item->last (full lifecycle wired in Commit E) --
      single-game/exhausted lists break out before dereferencing a stale ptr.

      COVERFLOW_PAD extra covers are built on EACH side, outside the visible slots. The slide
      translates the WHOLE strip by up to +/-coverDistance (animOffset, below) rather than moving
      covers between slots, so without padding the leading slot has nothing to slide in from: for
      the duration of every animation one edge slot was empty, while the cover at the far end was
      pushed entirely off-screen after being fetched and decoded.

      Indices are shifted by COVERFLOW_PAD so that i == COVERFLOW_PAD lands on the SAME screen
      position slot 0 used before (see posX in the draw loop, which subtracts it back out). Layout
      maths -- maxCoverWidth, coverSpacing, basePosX -- still keys off coverCount, the VISIBLE
      count, so nothing about the resting layout changes.
    */
    int coverTotal = coverCount + 2 * COVERFLOW_PAD; // entries built and drawn
    int centerIdx = COVERFLOW_PAD + centerIndex;     // the selection, within the padded array

    struct submenu_list *covers[COVERFLOW_MAX];
    int i;
    for (i = 0; i < COVERFLOW_MAX; i++)
        covers[i] = NULL;
    covers[centerIdx] = item;

    struct submenu_list *walk = item;
    for (i = centerIdx - 1; i >= 0; i--) {
        struct submenu_list *prev = walk->prev ? walk->prev : menu->item->last;
        if (!prev || prev == walk || prev == item)
            break;
        covers[i] = prev;
        walk = prev;
    }

    walk = item;
    for (i = centerIdx + 1; i < coverTotal; i++) {
        struct submenu_list *next = walk->next ? walk->next : menu->item->submenu;
        if (!next || next == walk || next == item)
            break;
        covers[i] = next;
        walk = next;
    }

    // Slide animation (cubic ease-out). clock() wrap is clamped to snap-complete.
    // Sign convention (FifthFox HW report: coverflow "advances in the opposite direction you'd expect"):
    // advancing (Next / Right, dir=+1) must bring the incoming cover in from the RIGHT -> the strip starts
    // shifted +right and eases to 0 (i.e. slides LEFT), and the OUTGOING old-center (now at centerIdx-dir)
    // is the cover that shrinks. The prior (eased-1.0f) / centerIdx+dir convention slid the opposite way.
    float eased = 1.0f;
    int animOffset = 0;
    if (gCoverflowAnimSpeed <= 0) {
        cfIsAnimating = 0;
    } else if (cfIsAnimating) {
        clock_t durTicks = (clock_t)(gCoverflowAnimSpeed * (CLOCKS_PER_SEC / 1000.0f));
        if (durTicks <= 0)
            durTicks = 1;
        clock_t elapsed = clock() - cfAnimStartTime;
        if (elapsed < 0)
            elapsed = durTicks; // clock wrap -> finish now
        float t = (float)elapsed / (float)durTicks;
        if (t >= 1.0f) {
            t = 1.0f;
            cfIsAnimating = 0;
        }
        eased = 1.0f - powf(1.0f - t, 3.0f);
        animOffset = (int)(cfAnimDirection * coverDistance * (1.0f - eased));
    }
    int leavingIndex = centerIdx - cfAnimDirection; // the OUTGOING old-center shrinks (opposite the slide-in side)

    rmSetReflectionYOffset(elem->reflectionOffset); // theme reflection_offset; reset after the loop

    /*
      At REST the two padding covers sit fully off-screen, so drawing them would cost two extra
      texture fetches per frame for nothing -- and this carousel is already the subject of a
      lag report (#271). Fetch and draw them ONLY while a slide is actually in flight, which is
      the only time they are on-screen. Rest-state cost is therefore byte-identical to before.
    */
    int drawFirst = cfIsAnimating ? 0 : COVERFLOW_PAD;
    int drawLast = cfIsAnimating ? coverTotal : coverTotal - COVERFLOW_PAD;

    for (i = drawFirst; i < drawLast; i++) {
        if (!covers[i])
            continue;

        // Per-cover element redirect: on the FAV tab an APP favourite uses the apps element (art
        // folder + case overlay); games and other tabs pass through to the coverflow element. The
        // texture lookup stays on sourceList (favList) so favGetImage proxies by the FAV index --
        // only the cache + overlay are redirected; frame-inset math uses THIS cover's own dims.
        theme_element_t *coverElem = thmGetElemForItem(menu, covers[i], elem);
        mutable_image_t *cimg = (mutable_image_t *)coverElem->extended;
        if (!cimg)
            continue;
        int csw = (coverElem->width > 0) ? coverElem->width : 1;   // div-guard
        int csh = (coverElem->height > 0) ? coverElem->height : 1; // div-guard

        GSTEXTURE *texture = getGameImageTexture(cimg->cache, sourceList, &covers[i]->item);
        int hasArt = (texture && texture->Mem);
        // #2 (Nadwislanski): a no-art Favourites entry (a favourited app / PS1 title / game with no
        // ART/<id>_COV.png) must NOT draw the embedded cover placeholder wrapped in the two-layer case
        // frame -- that reads as a hollow grey "empty tray" box. Skip the whole cover instead (draw
        // nothing), leaving the games/apps pages and real-art favourites untouched. Only while Cover
        // Art is ON: with gEnableArt off every favourite is "no art" and the carousel would go blank.
        if (gEnableArt && !hasArt && sourceList != NULL && sourceList->mode == FAV_MODE)
            continue;
        if (!hasArt)
            texture = cimg->defaultTexture ? &cimg->defaultTexture->source : thmGetTexture(COVER_DEFAULT);
        if (!texture || !texture->Mem)
            continue;

        // Center grows; the leaving neighbour shrinks. leavingIndex is bounds-checked into
        // [0,coverTotal) BEFORE it indexes covers[] (audit fix).
        int scaleAdd = 0;
        if (i == centerIdx)
            scaleAdd = (int)(gCoverflowCenterScale * eased);
        else if (cfIsAnimating && leavingIndex >= 0 && leavingIndex < coverTotal && i == leavingIndex)
            scaleAdd = (int)(gCoverflowCenterScale * (1.0f - eased));

        int drawW = coverWidth + scaleAdd;
        // Height tracks THIS cover's OWN element aspect (csh/csw), so a mixed tab -- Favourites,
        // which interleaves portrait game covers and square app covers -- honors each cover's shape
        // instead of stamping the base element's aspect on every cover. Width stays uniform
        // (coverWidth) for even carousel spacing. On single-type tabs coverElem == elem, so this is
        // identical to the old coverHeight*drawW/coverWidth. csw is div-guarded >= 1 above.
        int drawH = csh * drawW / csw;

        // Auto-center the VISIBLE cover (case frame box) from THIS cover's overlay corners: the
        // frame can sit off-center (e.g. apps_case = top-left 186 of 256). No overlay => no shift.
        int recenterX = 0, recenterY = 0;
        if (cimg->overlayTexture) {
            image_texture_t *ovc = cimg->overlayTexture;
            recenterX = csw / 2 - (ovc->upperLeft_x + ovc->upperRight_x) / 2;
            recenterY = csh / 2 - (ovc->upperLeft_y + ovc->lowerLeft_y) / 2;
        }

        int posX = basePosX + (i - COVERFLOW_PAD) * coverDistance + animOffset + recenterX * drawW / csw;
        // Covers draw ALIGN_CENTER at elem->posY, so a tall (portrait) cover's TOP overshoots a square
        // cover's top by (drawH-drawW)/2 and touches the background frame. Shift portrait covers DOWN by
        // that excess so every cover TOP-aligns at the square baseline -- square covers (Apps, square
        // VCD art) get 0 and are unchanged, and a mixed tab (Favourites) keeps a consistent cover top.
        int topAlign = (drawH > drawW) ? (drawH - drawW) / 2 : 0;
        int posY = elem->posY + recenterY * drawH / csh + topAlign;

        u64 coverColor = (gCoverflowDimCovers && i != centerIdx) ? GS_SETREG_RGBA(0x80, 0x80, 0x80, 0x40) : gDefaultCol;

        if (cimg->overlayTexture) {
            // Scale the inlay (cover) corner offsets to the drawn overlay size so the case
            // window tracks the cover at every scale. HW-verify alignment with real case art.
            image_texture_t *ov = cimg->overlayTexture;
            rmDrawOverlayPixmap(&ov->source, posX, posY, ALIGN_CENTER, drawW, drawH, SCALING_RATIO, coverColor,
                                texture, ov->upperLeft_x * drawW / csw, ov->upperLeft_y * drawH / csh, ov->upperRight_x * drawW / csw, ov->upperRight_y * drawH / csh,
                                ov->lowerLeft_x * drawW / csw, ov->lowerLeft_y * drawH / csh, ov->lowerRight_x * drawW / csw, ov->lowerRight_y * drawH / csh, elem->reflection);
            if (cimg->overlayTexture2)
                rmDrawPixmap(&cimg->overlayTexture2->source, posX, posY, ALIGN_CENTER, drawW, drawH, SCALING_RATIO, coverColor, elem->reflection);
        } else {
            rmDrawPixmap(texture, posX, posY, ALIGN_CENTER, drawW, drawH, SCALING_RATIO, coverColor, elem->reflection);
        }
    }

    rmSetReflectionYOffset(0); // don't leak the offset to any other reflection draw
}

static void initCoverflow(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name)
{
    mutable_image_t *mutableImage = initMutableImage(themePath, themeConfig, theme, name, elem->type, "COV", 10, NULL, NULL);
    elem->extended = mutableImage;
    elem->endElem = &endMutableImage;

    if (mutableImage && mutableImage->cache) {
        elem->drawElem = &drawCoverFlow;
        if (!theme->coverflow)
            theme->coverflow = elem; // first coverflow element = the "coverflow active" flag
    } else
        LOG("THEMES Coverflow %s: NO cache, elem disabled !!\n", name);
}

// AttributeImage ///////////////////////////////////////////////////////////////////////////////////////////////////////////

// Maps an AttributeImage's current value to the embedded internal glyph texId, including the
// "display/asset" suffix-value form ("NTSC/ntsc" with attribute "Vmode" -> "Vmode_ntsc").
// Returns -1 when no embedded glyph exists (e.g. Players, custom attributes); thmGetTexture(-1)
// safely yields NULL (unsigned wrap >= TEXTURES_COUNT).
static int thmAttributeTexId(mutable_image_t *attributeImage)
{
    char *seppos = strchr(attributeImage->currentValue, '/');
    if (!seppos)
        return texLookupInternalTexId(attributeImage->currentValue);

    char imgName[32];
    snprintf(imgName, sizeof(imgName), "%s_%s", attributeImage->cache->suffix, &seppos[1]);
    return texLookupInternalTexId(imgName);
}

static void drawAttributeImage(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    // No current item (empty list): clear, don't redraw the stale cached image -- same guard as
    // drawAttributeText/drawGameImage (#48). Also correct for the per-game #Media icon.
    if (item == NULL)
        return;
    mutable_image_t *attributeImage = (mutable_image_t *)elem->extended;
    if (config) {
        if (attributeImage->currentConfigId != config->uid) {
            // force refresh
            // Reset the cacheId TOO, and note this is mandatory rather than tidiness: the -2 memo gate
            // in cacheGetTexture short-circuits WITHOUT comparing the value string (unlike the identity
            // gate below it, which does strcmp entry->value). A -2 left over from the previous game
            // would therefore suppress THIS game's badge for the rest of the generation -- i.e. the
            // badge would stop updating / keep the old glyph. currentValue can only change inside this
            // same branch, so clearing both here is airtight.
            attributeImage->currentCacheId = -1;
            attributeImage->currentUid = -1;
            attributeImage->currentConfigId = config->uid;
            attributeImage->currentValue = NULL;
            configGetStr(config, attributeImage->cache->suffix, (const char **)&attributeImage->currentValue);
        }
        if (attributeImage->currentValue) {
            if (thmGetGuiValue() == 0) {
                GSTEXTURE *texture = thmGetTexture(thmAttributeTexId(attributeImage));
                if (texture && texture->Mem)
                    rmDrawPixmap(texture, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol, 0);

                return;
            } else {
                // Pass the PERSISTENT cacheId, not a stack local: cacheGetTexture's FAILED memo writes
                // *cacheId = -2 (+ *UID = gCacheGeneration) and its skip gate reads BOTH back next
                // frame. With a per-frame `int posZ = 0` the -2 was discarded, the gate was
                // unreachable, and every frame re-enqueued a fresh FAILING open -- the unbounded
                // info-page read storm behind #120/#154 (a badge whose glyph the theme does not ship
                // is the NORMAL case; that is why the embedded-glyph fallback above exists). This is
                // the same pattern getGameImageTexture has always used, which is exactly why main-page
                // covers/screenshots never wedged (AndrewBento's bisect, #154).
                GSTEXTURE *texture = cacheGetTexture(attributeImage->cache, menu->item->userdata, &attributeImage->currentCacheId, &attributeImage->currentUid, attributeImage->currentValue);
                if (texture && texture->Mem) {
                    if (attributeImage->overlayTexture) {
                        rmDrawOverlayPixmap(&attributeImage->overlayTexture->source, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol,
                                            texture, attributeImage->overlayTexture->upperLeft_x, attributeImage->overlayTexture->upperLeft_y, attributeImage->overlayTexture->upperRight_x, attributeImage->overlayTexture->upperRight_y,
                                            attributeImage->overlayTexture->lowerLeft_x, attributeImage->overlayTexture->lowerLeft_y, attributeImage->overlayTexture->lowerRight_x, attributeImage->overlayTexture->lowerRight_y, 0);
                        if (attributeImage->overlayTexture2)
                            rmDrawPixmap(&attributeImage->overlayTexture2->source, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol, 0);
                    } else
                        rmDrawPixmap(texture, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol, 0);

                    return;
                }
                // Embedded-glyph fallback: BUILT-IN theme only (#213). Attribute caches carry
                // prefix = themePath, so a NULL prefix is exactly the built-in path -- there the
                // baked glyph fills NULL frames while nothing else can. On a DISK theme this
                // fallback made our baked art FLASH before the theme's own glyph finished its async
                // load (and stand in permanently when the theme ships none) -- violating the rule
                // that a custom theme uses its OWN assets entirely. Disk themes now fall through to
                // the element's own _default (or draw nothing). thmLoad also no longer decodes the
                // baked set for disk themes, so this gate is double-covered.
                if (attributeImage->cache->prefix == NULL) {
                    texture = thmGetTexture(thmAttributeTexId(attributeImage));
                    if (texture && texture->Mem) {
                        rmDrawPixmap(texture, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol, 0);
                        return;
                    }
                }
            }
        }
    }
    if (attributeImage->defaultTexture)
        rmDrawPixmap(&attributeImage->defaultTexture->source, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol, 0);
}

static void initAttributeImage(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name)
{
    mutable_image_t *mutableImage = initMutableImage(themePath, themeConfig, theme, name, elem->type, NULL, 1, NULL, NULL);
    elem->extended = mutableImage;
    elem->endElem = &endMutableImage;

    if (mutableImage->cache)
        elem->drawElem = &drawAttributeImage;
    else
        LOG("THEMES AttributeImage %s: NO attribute, elem disabled !!\n", name);
}

// BasicElement /////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void endBasic(theme_element_t *elem)
{
    if (elem->extended)
        free(elem->extended);

    free(elem);
}

static theme_element_t *initBasic(const char *themePath, config_set_t *themeConfig, theme_t *theme, const char *name, int type, int x, int y, short aligned, int w, int h, short scaled, u64 color, int font)
{
    int intValue;
    unsigned char charColor[3];
    const char *temp;
    char elemProp[64];

    theme_element_t *elem = (theme_element_t *)malloc(sizeof(theme_element_t));

    elem->type = type;
    elem->reflection = 0;
    elem->reflectionOffset = 0;
    elem->deviceFilter = 0;   // malloc'd without memset: MUST be zeroed explicitly (0 = unfiltered)
    elem->deviceCoverage = 0; // filled at validateGUIElems for unfiltered MenuIcon/ItemsList/HintText
    elem->extended = NULL;
    elem->drawElem = NULL;
    elem->endElem = &endBasic;
    elem->next = NULL;

    snprintf(elemProp, sizeof(elemProp), "%s_x", name);
    if (configGetStr(themeConfig, elemProp, &temp)) {
        if (!strncmp(temp, "POS_MID", 7))
            x = screenWidth >> 1;
        else
            x = atoi(temp);
    }
    if (x < 0)
        elem->posX = screenWidth + x;
    else
        elem->posX = x;

    snprintf(elemProp, sizeof(elemProp), "%s_y", name);
    if (configGetStr(themeConfig, elemProp, &temp)) {
        if (!strncmp(temp, "POS_MID", 7))
            y = screenHeight >> 1;
        else
            y = atoi(temp);
    }
    if (y < 0)
        elem->posY = ceil((screenHeight + y) * theme->usedHeight / screenHeight);
    else
        elem->posY = y;

    snprintf(elemProp, sizeof(elemProp), "%s_width", name);
    if (configGetStr(themeConfig, elemProp, &temp)) {
        if (!strncmp(temp, "DIM_INF", 7))
            elem->width = screenWidth;
        else
            elem->width = atoi(temp);
    } else
        elem->width = w;

    snprintf(elemProp, sizeof(elemProp), "%s_height", name);
    if (configGetStr(themeConfig, elemProp, &temp)) {
        if (!strncmp(temp, "DIM_INF", 7))
            elem->height = screenHeight;
        else
            elem->height = atoi(temp);
    } else
        elem->height = h;

    snprintf(elemProp, sizeof(elemProp), "%s_aligned", name);
    if (configGetInt(themeConfig, elemProp, &intValue)) {
        // 0 = none/left, 2 = right-justified (wOPL/uOPL theme key; used to silently center),
        // anything else = centered (the long-standing non-zero behavior).
        if (intValue == 0)
            elem->aligned = ALIGN_NONE;
        else if (intValue == 2)
            elem->aligned = (ALIGN_VCENTER | ALIGN_RIGHT);
        else
            elem->aligned = ALIGN_CENTER;
    } else
        elem->aligned = aligned;

    snprintf(elemProp, sizeof(elemProp), "%s_scaled", name);
    if (configGetInt(themeConfig, elemProp, &intValue))
        elem->scaled = (intValue == 0) ? SCALING_NONE : SCALING_RATIO;
    else
        elem->scaled = scaled;

    snprintf(elemProp, sizeof(elemProp), "%s_color", name);
    if (configGetColor(themeConfig, elemProp, charColor))
        elem->color = GS_SETREG_RGBA(charColor[0], charColor[1], charColor[2], 0x80);
    else
        elem->color = color;

    elem->font = font;
    snprintf(elemProp, sizeof(elemProp), "%s_font", name);
    if (configGetInt(themeConfig, elemProp, &intValue)) {
        if (intValue > 0 && intValue < THM_MAX_FONTS)
            elem->font = theme->fonts[intValue];
    }

    snprintf(elemProp, sizeof(elemProp), "%s_reflection", name);
    if (configGetInt(themeConfig, elemProp, &intValue))
        elem->reflection = intValue ? 1 : 0;

    snprintf(elemProp, sizeof(elemProp), "%s_reflection_offset", name);
    if (configGetInt(themeConfig, elemProp, &intValue)) {
        // Clamp to a sane band: an offset beyond a screen height only draws strips off-canvas
        // (wasted work), and a hand-edited extreme should not push the reflection into nowhere.
        if (intValue < -1024)
            intValue = -1024;
        else if (intValue > 1024)
            intValue = 1024;
        elem->reflectionOffset = intValue;
    }

    // Optional per-device filter (MenuIcon/ItemsList/HintText consume it; harmless elsewhere).
    snprintf(elemProp, sizeof(elemProp), "%s_devices", name);
    if (configGetStr(themeConfig, elemProp, &temp))
        elem->deviceFilter = thmParseDeviceList(temp, 0);

    return elem;
}

// Internal elements ////////////////////////////////////////////////////////////////////////////////////////////////////////
static void drawBackground(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    guiDrawBGPlasma();
}

static void initBackground(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name, const char *pattern, int count, const char *texture)
{
    mutable_image_t *mutableImage = initMutableImage(themePath, themeConfig, theme, name, elem->type, pattern, count, texture, NULL);
    elem->extended = mutableImage;
    elem->endElem = &endMutableImage;

    if (mutableImage->cache)
        elem->drawElem = &drawGameImage;
    else if (mutableImage->defaultTexture)
        elem->drawElem = &drawStaticImage;
    else
        elem->drawElem = &drawBackground;
}

static void drawMenuIcon(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    if (thmElemSkipsDevice(elem, menu->item->icon_id))
        return; // devices= filter: not this page's element
    GSTEXTURE *menuIconTex = thmGetTexture(menu->item->icon_id);
    if (menuIconTex && menuIconTex->Mem)
        rmDrawPixmap(menuIconTex, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol, 0);
}

static int findMenuNext(struct menu_list *menu)
{
    struct menu_list *next = menu->next;
    while (next != NULL && next->item->visible == 0)
        next = next->next;

    return next == NULL ? 0 : next->item->visible;
}

static int findMenuPrev(struct menu_list *menu)
{
    struct menu_list *prev = menu->prev;
    while (prev != NULL && prev->item->visible == 0)
        prev = prev->prev;

    return prev == NULL ? 0 : prev->item->visible;
}

static void drawMenuText(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    GSTEXTURE *leftIconTex = NULL, *rightIconTex = NULL;
    if (findMenuPrev(menu) != 0)
        leftIconTex = thmGetTexture(LEFT_ICON);
    if (findMenuNext(menu) != 0)
        rightIconTex = thmGetTexture(RIGHT_ICON);

    if (elem->aligned) {
        int offset = elem->width >> 1;
        if (leftIconTex && leftIconTex->Mem)
            rmDrawPixmap(leftIconTex, elem->posX - offset, elem->posY, elem->aligned, 20, 20, elem->scaled, gDefaultCol, 0);
        if (rightIconTex && rightIconTex->Mem)
            rmDrawPixmap(rightIconTex, elem->posX + offset, elem->posY, elem->aligned, 20, 20, elem->scaled, gDefaultCol, 0);
    } else {
        if (leftIconTex && leftIconTex->Mem)
            rmDrawPixmap(leftIconTex, elem->posX - leftIconTex->Width, elem->posY, elem->aligned, 20, 20, elem->scaled, gDefaultCol, 0);
        if (rightIconTex && rightIconTex->Mem)
            rmDrawPixmap(rightIconTex, elem->posX + elem->width, elem->posY, elem->aligned, 20, 20, elem->scaled, gDefaultCol, 0);
    }
    fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, 0, 0, menuItemGetText(menu->item), elem->color);
}

static void drawBDMIndex(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    item_list_t *itemList = menu->item->userdata;
    // Only render for bdm modes and if current mode is visible
    if (itemList->mode >= ETH_MODE || menu->item->visible == 0)
        return;

    // Only render if multiple mass devices are connected
    if (itemList->mode == 0 && menu->next->item->visible == 0)
        return;

    char imgName[32];
    snprintf(imgName, sizeof(imgName), "Index_%d", itemList->mode);

    GSTEXTURE *indexTex = thmGetTexture(texLookupInternalTexId(&imgName[0]));
    if (indexTex && indexTex->Mem)
        rmDrawPixmap(indexTex, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol, 0);
}

static void drawItemsList(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    // devices= filter: the nav pointer (gTheme->itemsList) is resolved with the SAME matching rules
    // (thmResolveItemsList), so the rows that draw are always the rows navigation counts.
    if (thmElemSkipsDevice(elem, menu->item->icon_id))
        return;

    if (item) {
        items_list_t *itemsList = (items_list_t *)elem->extended;
        item_list_t *list = menu->item->userdata;
        int mmceSelectionChanged = 0;
        char *selectedStartup = NULL;

        if (list != NULL && list->mode == MMCE_MODE && itemsList->decoratorImage != NULL && itemsList->decoratorImage->cache != NULL) {
            selectedStartup = list->itemGetStartup(list, item->item.id);
            if (selectedStartup == NULL)
                selectedStartup = "";

            if (itemsList->lastSelectedItemId != item->item.id || strcmp(itemsList->lastSelectedStartup, selectedStartup) != 0) {
                mmceSelectionChanged = 1;
                itemsList->lastSelectedItemId = item->item.id;
                snprintf(itemsList->lastSelectedStartup, sizeof(itemsList->lastSelectedStartup), "%s", selectedStartup);
            }
        }

        int posX = elem->posX, posY = elem->posY;
        if (elem->aligned) {
            posX -= elem->width >> 1;
            posY -= elem->height >> 1;
        }

        submenu_list_t *ps = menu->item->pagestart;
        int others = 0;
        u64 color;
        int textEndX = 0;
        while (ps && (others++ < itemsList->displayedItems)) {
            if (ps == item)
                color = gTheme->selTextColor;
            else
                color = elem->color;

            // Folder browsing: a folder row shows its name with a trailing "/" and is NEVER routed
            // through the cover cache (it has no startup key -- doing so would thrash the cache and
            // paint the empty case frame). The default decorator slot keeps the text aligned with games.
            const char *dispText = vcdDisplayName(list ? list->mode : -1, submenuItemGetText(&ps->item));
            char folderBuf[256];
            if (ps->item.isFolder) {
                snprintf(folderBuf, sizeof(folderBuf), "%s/", dispText);
                dispText = folderBuf;
            }

            if (itemsList->decoratorImage) {
                GSTEXTURE *itemIconTex = NULL;

                if (ps->item.isFolder) {
                    itemIconTex = NULL; // folders never carry a cover
                } else if (list != NULL && list->mode == MMCE_MODE && itemsList->decoratorImage->cache != NULL) {
                    image_cache_t *cache = itemsList->decoratorImage->cache;

                    if (mmceSelectionChanged && ps == item)
                        itemIconTex = getGameImageTexture(cache, menu->item->userdata, &ps->item);
                    else
                        itemIconTex = cacheGetTextureIfReady(cache, &ps->item.cache_id[cache->userId], &ps->item.cache_uid[cache->userId]);
                } else
                    itemIconTex = getGameImageTexture(itemsList->decoratorImage->cache, menu->item->userdata, &ps->item);

                if (itemIconTex && itemIconTex->Mem)
                    rmDrawPixmap(itemIconTex, posX, posY, elem->aligned, DECORATOR_SIZE, DECORATOR_SIZE, elem->scaled, gDefaultCol, 0);
                else {
                    if (itemsList->decoratorImage->defaultTexture)
                        rmDrawPixmap(&itemsList->decoratorImage->defaultTexture->source, posX, posY, elem->aligned, DECORATOR_SIZE, DECORATOR_SIZE, elem->scaled, gDefaultCol, 0);
                }
                textEndX = fntRenderString(elem->font, elem->posX + DECORATOR_SIZE, posY, elem->aligned, elem->width, elem->height, dispText, color);
            } else
                textEndX = fntRenderString(elem->font, elem->posX, posY, elem->aligned, elem->width, elem->height, dispText, color);

            // Favourites: draw a small star just after the item text.
            if (ps->item.favourited) {
                GSTEXTURE *favTex = thmGetTexture(FAV_MARK);
                if (favTex != NULL && favTex->Mem != NULL)
                    rmDrawPixmap(favTex, textEndX + 4, posY, elem->aligned, MENU_ITEM_HEIGHT, MENU_ITEM_HEIGHT, elem->scaled, gDefaultCol, 0);
            }

            posY += MENU_ITEM_HEIGHT;
            ps = ps->next;
        }
    }
}

static void initItemsList(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name, const char *decorator)
{
    char elemProp[64];

    items_list_t *itemsList = (items_list_t *)malloc(sizeof(items_list_t));

    if (elem->width == DIM_UNDEF)
        elem->width = screenWidth;

    if (elem->height == DIM_UNDEF)
        elem->height = theme->usedHeight - (MENU_POS_V + HINT_HEIGHT);

    itemsList->displayedItems = elem->height / MENU_ITEM_HEIGHT;
    LOG("THEMES ItemsList %s: displaying %d elems, item height: %d\n", name, itemsList->displayedItems, elem->height);

    itemsList->decorator = NULL;
    snprintf(elemProp, sizeof(elemProp), "%s_decorator", name);
    configGetStr(themeConfig, elemProp, &decorator);
    if (decorator)
        itemsList->decorator = decorator; // Will be used later (thmValidate)

    itemsList->decoratorImage = NULL;
    itemsList->lastSelectedItemId = -1;
    itemsList->lastSelectedStartup[0] = '\0';

    elem->extended = itemsList;
    // elem->endElem = &endBasic; does the job

    elem->drawElem = &drawItemsList;
}

static void drawItemText(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    if (item) {
        item_list_t *support = menu->item->userdata;
        // In a VCD view itemGetStartup returns the (full) VCD name, so this is the name caption some
        // CoverFlow/custom themes use instead of an ItemsList. Apply the same display-only game-ID
        // hide (no-op for non-VCD views, where this shows the real startup/game-ID). Cosmetic only.
        fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, 0, 0, vcdDisplayName(support->mode, support->itemGetStartup(support, item->item.id)), elem->color);
    }
}

static void drawHintText(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    if (thmElemSkipsDevice(elem, menu->item->icon_id))
        return; // devices= filter: not this page's element

    menu_hint_item_t *hint = menu->item->hints;
    if (hint) {
        int x = elem->posX;

        if (elem->aligned)
            x = guiAlignMenuHints(hint, elem->font, elem->width);

        for (; hint; hint = hint->next) {
            x = guiDrawIconAndText(hint->icon_id, hint->text_id, elem->font, x, elem->posY, elem->color);
            x += elem->width;
        }
    }
}

static void drawInfoHintText(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    int infoHints[2] = {_STR_RUN, _STR_BACK};
    int infoIcons[2] = {CIRCLE_ICON, CROSS_ICON};
    int x = elem->posX;

    if (elem->aligned)
        x = guiAlignSubMenuHints(2, infoHints, infoIcons, elem->font, elem->width, 1);

    x = guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? infoIcons[0] : infoIcons[1], infoHints[0], elem->font, x, elem->posY, elem->color);
    x += elem->width;
    x = guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? infoIcons[1] : infoIcons[0], infoHints[1], elem->font, x, elem->posY, elem->color);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void validateBackgroundElems(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_elems_t *mainElems, theme_elems_t *infoElems)
{
    if (!mainElems->first || (mainElems->first->type != ELEM_TYPE_BACKGROUND)) {
        LOG("THEMES No valid background found for main, add default BG_ART\n");
        theme_element_t *backgroundElem = initBasic(themePath, themeConfig, theme, "bg", ELEM_TYPE_BACKGROUND, 0, 0, ALIGN_NONE, screenWidth, screenHeight, SCALING_NONE, gDefaultCol, theme->fonts[0]);
        initBackground(themePath, themeConfig, theme, backgroundElem, "bg", "BG", 1, NULL);
        backgroundElem->next = mainElems->first;
        mainElems->first = backgroundElem;
    }

    if (infoElems->first) {
        if (infoElems->first->type != ELEM_TYPE_BACKGROUND) {
            LOG("THEMES No valid background found for info, add default BG_ART\n");
            theme_element_t *backgroundElem = initBasic(themePath, themeConfig, theme, "bg", ELEM_TYPE_BACKGROUND, 0, 0, ALIGN_NONE, screenWidth, screenHeight, SCALING_NONE, gDefaultCol, theme->fonts[0]);
            initBackground(themePath, themeConfig, theme, backgroundElem, "bg", "BG", 1, NULL);
            backgroundElem->next = infoElems->first;
            infoElems->first = backgroundElem;
        }
    }
}

// Returns the validated list -- the caller MUST store it back into the slot: when no list exists a
// default one is created here, and before the write-back the slot stayed NULL while the default was
// only chained for drawing, leaving gTheme->itemsList NULL (menusys derefs it without a check). That
// was unreachable while every theme carried an unfiltered ItemsList; devices=-filtered ItemsLists
// (which deliberately claim no slot) make it a one-key theme edit away.
static theme_element_t *validateItemsList(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *list, theme_elems_t *mainElems)
{
    if (list) {
        items_list_t *itemsList = (items_list_t *)list->extended;
        if (itemsList->decorator) {
            // Second pass to find the decorator
            theme_element_t *decoratorElem = mainElems->first;
            while (decoratorElem) {
                if (decoratorElem->type == ELEM_TYPE_GAME_IMAGE) {
                    mutable_image_t *gameImage = (mutable_image_t *)decoratorElem->extended;
                    // A GAME_IMAGE element can have cache == NULL (no _pattern); guard before
                    // dereferencing ->suffix, matching drawGameImage and the other cache users.
                    if (gameImage->cache && !strcmp(itemsList->decorator, gameImage->cache->suffix)) {
                        // if user want to cache less than displayed items, then disable itemslist icons, if not would load constantly
                        if (gameImage->cache->count >= itemsList->displayedItems)
                            itemsList->decoratorImage = gameImage;
                        break;
                    }
                }

                decoratorElem = decoratorElem->next;
            }
            itemsList->decorator = NULL;
        }
    } else {
        LOG("THEMES No itemsList found, adding a default one\n");
        list = initBasic(themePath, themeConfig, theme, "il", ELEM_TYPE_ITEMS_LIST, 42, 42, ALIGN_NONE, 373, 316, SCALING_RATIO, theme->textColor, theme->fonts[0]);
        initItemsList(themePath, themeConfig, theme, list, "il", NULL);
        list->next = mainElems->first->next; // Position the itemsList as second element (right after the Background)
        mainElems->first->next = list;
    }

    return list;
}

// Run validateItemsList's decorator linking for devices=-FILTERED ItemsList elements: they claim no
// slot, so the slot-based pass above never sees them -- without this their `decorator` string would
// keep pointing into themeConfig (freed at the end of thmLoad) and the decorator image would never
// link. Filtered lists are always non-NULL here, so this can never take the create-default branch.
static void validateFilteredItemsLists(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_elems_t *mainElems)
{
    theme_element_t *elem;

    for (elem = mainElems->first; elem != NULL; elem = elem->next) {
        if (elem->type == ELEM_TYPE_ITEMS_LIST && elem->deviceFilter)
            validateItemsList(themePath, themeConfig, theme, elem, mainElems);
    }
}

static int isDecoratorCoverCache(theme_element_t *list, image_cache_t *cache)
{
    items_list_t *itemsList;

    if (list == NULL || list->extended == NULL || cache == NULL)
        return 0;

    itemsList = (items_list_t *)list->extended;
    return itemsList->decoratorImage != NULL && itemsList->decoratorImage->cache == cache;
}

// devices=-filtered ItemsLists own no slot, so the four slot checks in isDecoratorCoverImage never
// see their decorators. Recognizing them here is the CASCADE KILLER for the filtered-decorator
// cache split below: without it, each split's replaceSharedCoverCache treats another filtered
// list's decorator as a plain selected cover and re-points it, so repeated splits keep cloning.
// Walks all 8 families; keys off elem->deviceFilter (0 for every element of a devices=-free theme,
// so such themes take zero new branches).
static int isFilteredDecoratorCoverImage(theme_t *theme, mutable_image_t *gameImage)
{
    theme_elems_t *groups[8] = {&theme->mainElems, &theme->infoElems, &theme->appsMainElems, &theme->appsInfoElems,
                                &theme->favsMainElems, &theme->favsInfoElems, &theme->vcdMainElems, &theme->vcdInfoElems};
    int g;

    for (g = 0; g < 8; g++) {
        theme_element_t *e = groups[g]->first;
        while (e != NULL) {
            if (e->type == ELEM_TYPE_ITEMS_LIST && e->deviceFilter && e->extended != NULL &&
                ((items_list_t *)e->extended)->decoratorImage == gameImage)
                return 1;
            e = e->next;
        }
    }
    return 0;
}

static int isDecoratorCoverImage(theme_t *theme, mutable_image_t *gameImage)
{
    items_list_t *itemsList;

    if (theme == NULL || gameImage == NULL)
        return 0;

    if (theme->gamesItemsList != NULL && theme->gamesItemsList->extended != NULL) {
        itemsList = (items_list_t *)theme->gamesItemsList->extended;
        if (itemsList->decoratorImage == gameImage)
            return 1;
    }

    if (theme->appsItemsList != NULL && theme->appsItemsList->extended != NULL) {
        itemsList = (items_list_t *)theme->appsItemsList->extended;
        if (itemsList->decoratorImage == gameImage)
            return 1;
    }

    if (theme->favsItemsList != NULL && theme->favsItemsList->extended != NULL) {
        itemsList = (items_list_t *)theme->favsItemsList->extended;
        if (itemsList->decoratorImage == gameImage)
            return 1;
    }

    if (theme->vcdItemsList != NULL && theme->vcdItemsList->extended != NULL) {
        itemsList = (items_list_t *)theme->vcdItemsList->extended;
        if (itemsList->decoratorImage == gameImage)
            return 1;
    }

    return isFilteredDecoratorCoverImage(theme, gameImage);
}

static image_cache_t *cloneImageCache(theme_t *theme, image_cache_t *source)
{
    image_cache_t *cache;

    if (theme == NULL || source == NULL)
        return NULL;

    cache = cacheInitCache(theme->gameCacheCount++, source->prefix, source->isPrefixRelative, source->suffix, source->count);
    if (cache != NULL)
        cache->allowPrime = source->allowPrime;

    return cache;
}

static void replaceSharedCoverCache(theme_t *theme, theme_elems_t *elems, image_cache_t *sourceCache, image_cache_t *replacementCache, int *replacementAssigned)
{
    theme_element_t *elem;

    if (theme == NULL || elems == NULL || sourceCache == NULL || replacementCache == NULL || replacementAssigned == NULL)
        return;

    elem = elems->first;
    while (elem != NULL) {
        if (elem->type == ELEM_TYPE_GAME_IMAGE) {
            mutable_image_t *gameImage = (mutable_image_t *)elem->extended;

            if (gameImage != NULL && !isDecoratorCoverImage(theme, gameImage) && gameImage->cache == sourceCache) {
                gameImage->cache = replacementCache;
                gameImage->cacheLinked = *replacementAssigned ? 1 : 0;
                *replacementAssigned = 1;
            }
        }

        elem = elem->next;
    }
}

static void splitDecoratorCoverCache(theme_t *theme, theme_element_t *list)
{
    items_list_t *itemsList;
    image_cache_t *sourceCache;
    image_cache_t *replacementCache;
    int replacementAssigned;

    if (theme == NULL || list == NULL || list->extended == NULL)
        return;

    itemsList = (items_list_t *)list->extended;
    if (itemsList->decoratorImage == NULL || itemsList->decoratorImage->cache == NULL)
        return;

    sourceCache = itemsList->decoratorImage->cache;
    if (sourceCache->suffix == NULL || strcmp(sourceCache->suffix, "COV") != 0)
        return;

    replacementCache = cloneImageCache(theme, sourceCache);
    if (replacementCache == NULL)
        return;

    replacementAssigned = 0;
    replaceSharedCoverCache(theme, &theme->mainElems, sourceCache, replacementCache, &replacementAssigned);
    replaceSharedCoverCache(theme, &theme->infoElems, sourceCache, replacementCache, &replacementAssigned);
    replaceSharedCoverCache(theme, &theme->appsMainElems, sourceCache, replacementCache, &replacementAssigned);
    replaceSharedCoverCache(theme, &theme->appsInfoElems, sourceCache, replacementCache, &replacementAssigned);
    replaceSharedCoverCache(theme, &theme->favsMainElems, sourceCache, replacementCache, &replacementAssigned);
    replaceSharedCoverCache(theme, &theme->favsInfoElems, sourceCache, replacementCache, &replacementAssigned);
    replaceSharedCoverCache(theme, &theme->vcdMainElems, sourceCache, replacementCache, &replacementAssigned);
    replaceSharedCoverCache(theme, &theme->vcdInfoElems, sourceCache, replacementCache, &replacementAssigned);

    if (!replacementAssigned)
        cacheDestroyCache(replacementCache);
}

// Run the decorator/selected-cover split for every devices=-filtered ItemsList (they own no slot,
// so the four slot calls in validateGUIElems never reach them). splitDecoratorCoverCache's own
// guards (NULL extended/decoratorImage, non-COV suffix, destroy-unassigned-clone) make repeat
// calls per shared source cache a no-op -- see the non-cascade note at the call site.
static void splitFilteredDecoratorCoverCaches(theme_t *theme)
{
    int g;

    if (theme == NULL)
        return;

    theme_elems_t *groups[8] = {&theme->mainElems, &theme->infoElems, &theme->appsMainElems, &theme->appsInfoElems,
                                &theme->favsMainElems, &theme->favsInfoElems, &theme->vcdMainElems, &theme->vcdInfoElems};
    for (g = 0; g < 8; g++) {
        theme_element_t *e = groups[g]->first;
        while (e != NULL) {
            if (e->type == ELEM_TYPE_ITEMS_LIST && e->deviceFilter)
                splitDecoratorCoverCache(theme, e);
            e = e->next;
        }
    }
}

// Cache-based twin of isFilteredDecoratorCoverImage for the clamp below: true when `cache` backs a
// devices=-filtered ItemsList's decorator in ANY family. Zero-cost for devices=-free themes.
static int isFilteredDecoratorCoverCache(theme_t *theme, image_cache_t *cache)
{
    int g;

    if (theme == NULL || cache == NULL) // unreachable from the current callers; matches the sibling helpers' guards (Gemini, #168)
        return 0;

    theme_elems_t *groups[8] = {&theme->mainElems, &theme->infoElems, &theme->appsMainElems, &theme->appsInfoElems,
                                &theme->favsMainElems, &theme->favsInfoElems, &theme->vcdMainElems, &theme->vcdInfoElems};

    for (g = 0; g < 8; g++) {
        theme_element_t *e = groups[g]->first;
        while (e != NULL) {
            if (e->type == ELEM_TYPE_ITEMS_LIST && e->deviceFilter && isDecoratorCoverCache(e, cache))
                return 1;
            e = e->next;
        }
    }
    return 0;
}

static void clampSelectedCoverCaches(theme_t *theme, theme_elems_t *elems)
{
    theme_element_t *elem = elems->first;

    while (elem != NULL) {
        if (elem->type == ELEM_TYPE_GAME_IMAGE) {
            mutable_image_t *gameImage = (mutable_image_t *)elem->extended;

            if (gameImage != NULL && gameImage->cache != NULL && gameImage->cache->suffix != NULL && strcmp(gameImage->cache->suffix, "COV") == 0 &&
                !isDecoratorCoverCache(theme->gamesItemsList, gameImage->cache) && !isDecoratorCoverCache(theme->appsItemsList, gameImage->cache) &&
                !isDecoratorCoverCache(theme->favsItemsList, gameImage->cache) && !isDecoratorCoverCache(theme->vcdItemsList, gameImage->cache) &&
                !isFilteredDecoratorCoverCache(theme, gameImage->cache)) {
                gameImage->cache->allowPrime = 0;
            }
        }

        elem = elem->next;
    }
}

// True if `cache` is referenced by a COV GAME_IMAGE OUTSIDE the vcd family (games/apps/favs main+info).
// separateVcdCoverCache only acts when the vcd covers' cache is genuinely SHARED with a non-vcd family;
// if the cache is vcd-private (e.g. a theme with vcd COV covers but no games COV cover), cloning and
// re-pointing the vcd covers would leave the original cache unreferenced -> a one-time leak. Skip then.
static int vcdCoverCacheSharedOutsideVcd(theme_t *theme, image_cache_t *cache)
{
    theme_elems_t *groups[6] = {&theme->mainElems, &theme->infoElems, &theme->appsMainElems,
                                &theme->appsInfoElems, &theme->favsMainElems, &theme->favsInfoElems};
    for (int g = 0; g < 6; g++) {
        theme_element_t *e = groups[g]->first;
        while (e != NULL) {
            if (e->type == ELEM_TYPE_GAME_IMAGE) {
                mutable_image_t *gi = (mutable_image_t *)e->extended;
                if (gi != NULL && gi->cache == cache)
                    return 1;
            }
            e = e->next;
        }
    }
    return 0;
}

// Give the L3 VCD/PS1 view its OWN COV cover cache so it never shares cache slots with the games view.
// The VCD list reuses the device's own game list (identical submenu item ids), and findDuplicate unifies
// every "COV" GAME_IMAGE cache by suffix -- so without this the VCD covers and the ISO covers collide in
// one (cache userId, item id)-keyed cache and thrash on every L3 toggle (eliminator1403's no-covers /
// flash-then-wrong-cover hardware report). splitDecoratorCoverCache only separates DECORATOR covers and
// no-ops on themes whose items list has no decorator (e.g. the default Coverflow theme), so the selected/
// carousel cover (vcdMain2) would still share the games cache; this handles that case unconditionally.
// Clones the COV cache used by the vcd family and re-points its non-decorator COV game-images onto it.
static void separateVcdCoverCache(theme_t *theme)
{
    if (theme == NULL)
        return;

    image_cache_t *source = NULL;
    theme_element_t *elem = theme->vcdMainElems.first;
    while (elem != NULL) {
        if (elem->type == ELEM_TYPE_GAME_IMAGE) {
            mutable_image_t *gameImage = (mutable_image_t *)elem->extended;
            if (gameImage != NULL && gameImage->cache != NULL && gameImage->cache->suffix != NULL &&
                strcmp(gameImage->cache->suffix, "COV") == 0 && !isDecoratorCoverImage(theme, gameImage)) {
                source = gameImage->cache; // the COV cache shared with the games view (via findDuplicate)
                break;
            }
        }
        elem = elem->next;
    }
    // Only separate when the cache is genuinely shared with a non-vcd family. If it is vcd-private there
    // is nothing to separate, and cloning + re-pointing would orphan (leak) the original cache.
    if (source == NULL || !vcdCoverCacheSharedOutsideVcd(theme, source))
        return;

    image_cache_t *replacement = cloneImageCache(theme, source);
    if (replacement == NULL)
        return;

    int replacementAssigned = 0;
    replaceSharedCoverCache(theme, &theme->vcdMainElems, source, replacement, &replacementAssigned);
    replaceSharedCoverCache(theme, &theme->vcdInfoElems, source, replacement, &replacementAssigned);

    if (!replacementAssigned)
        cacheDestroyCache(replacement);
}

static void validateGUIElems(const char *themePath, config_set_t *themeConfig, theme_t *theme)
{
    // 1. check we have a valid Background elements
    validateBackgroundElems(themePath, themeConfig, theme, &theme->mainElems, &theme->infoElems);
    validateBackgroundElems(themePath, themeConfig, theme, &theme->appsMainElems, &theme->appsInfoElems);
    validateBackgroundElems(themePath, themeConfig, theme, &theme->favsMainElems, &theme->favsInfoElems);
    validateBackgroundElems(themePath, themeConfig, theme, &theme->vcdMainElems, &theme->vcdInfoElems);

    // 2. check we have a valid ItemsList element, and link its decorator to the target element.
    // Store the result back: validateItemsList may CREATE the default list, and a NULL slot is a
    // menusys NULL deref (see the function comment).
    theme->gamesItemsList = validateItemsList(themePath, themeConfig, theme, theme->gamesItemsList, &theme->mainElems);
    theme->appsItemsList = validateItemsList(themePath, themeConfig, theme, theme->appsItemsList, &theme->appsMainElems);
    theme->favsItemsList = validateItemsList(themePath, themeConfig, theme, theme->favsItemsList, &theme->favsMainElems);
    theme->vcdItemsList = validateItemsList(themePath, themeConfig, theme, theme->vcdItemsList, &theme->vcdMainElems);

    // devices=-filtered ItemsList overrides: link their decorators (they own no slot, so the pass
    // above never reaches them). Info families too -- a filtered ItemsList can be declared there
    // and its decorator string must not be left pointing into themeConfig (freed at end of load).
    // Their decorators are ALSO cache-split (splitFilteredDecoratorCoverCaches below) and
    // clamp-exempt (isFilteredDecoratorCoverCache), the same treatment the slot lists get --
    // isDecoratorCoverImage recognizes them, which is what keeps repeated splits from cascading.
    validateFilteredItemsLists(themePath, themeConfig, theme, &theme->mainElems);
    validateFilteredItemsLists(themePath, themeConfig, theme, &theme->infoElems);
    validateFilteredItemsLists(themePath, themeConfig, theme, &theme->appsMainElems);
    validateFilteredItemsLists(themePath, themeConfig, theme, &theme->appsInfoElems);
    validateFilteredItemsLists(themePath, themeConfig, theme, &theme->favsMainElems);
    validateFilteredItemsLists(themePath, themeConfig, theme, &theme->favsInfoElems);
    validateFilteredItemsLists(themePath, themeConfig, theme, &theme->vcdMainElems);
    validateFilteredItemsLists(themePath, themeConfig, theme, &theme->vcdInfoElems);

    // ...then precompute the unfiltered elements' coverage so an unfiltered MenuIcon/ItemsList/
    // HintText yields to filtered same-type siblings on the devices those cover. Must run AFTER the
    // slot pass: the auto-created default list needs its coverage too.
    thmComputeDeviceCoverage(&theme->mainElems);
    thmComputeDeviceCoverage(&theme->infoElems);
    thmComputeDeviceCoverage(&theme->appsMainElems);
    thmComputeDeviceCoverage(&theme->appsInfoElems);
    thmComputeDeviceCoverage(&theme->favsMainElems);
    thmComputeDeviceCoverage(&theme->favsInfoElems);
    thmComputeDeviceCoverage(&theme->vcdMainElems);
    thmComputeDeviceCoverage(&theme->vcdInfoElems);

    // Items-list decorator covers need their own cache; sharing with selected covers defeats MMCE cover clamping.
    splitDecoratorCoverCache(theme, theme->gamesItemsList);
    splitDecoratorCoverCache(theme, theme->appsItemsList);
    splitDecoratorCoverCache(theme, theme->favsItemsList);
    splitDecoratorCoverCache(theme, theme->vcdItemsList);

    // devices=-filtered lists' decorators get the same split. Safe to repeat per list: with
    // isDecoratorCoverImage recognizing every decorator (filtered included), a split's
    // replaceSharedCoverCache can never re-point another list's decorator, so after the first
    // effective clone per source cache the later calls find no sharers and destroy their clone.
    splitFilteredDecoratorCoverCaches(theme);

    // The L3 VCD view reuses the device's own game list (same item ids), so its selected/carousel covers
    // must not share a COV cache with the ISO list -- otherwise toggling thrashes the same cache slots.
    separateVcdCoverCache(theme);

    // Selected-cover caches do not need history unless a real items list decorator uses them.
    clampSelectedCoverCaches(theme, &theme->mainElems);
    clampSelectedCoverCaches(theme, &theme->infoElems);
    clampSelectedCoverCaches(theme, &theme->appsMainElems);
    clampSelectedCoverCaches(theme, &theme->appsInfoElems);
    clampSelectedCoverCaches(theme, &theme->favsMainElems);
    clampSelectedCoverCaches(theme, &theme->favsInfoElems);
    clampSelectedCoverCaches(theme, &theme->vcdMainElems);
    clampSelectedCoverCaches(theme, &theme->vcdInfoElems);
}

static int addGUIElem(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_elems_t *elems, const char *type, const char *name)
{
    int enabled = 1;
    char elemProp[64];
    theme_element_t *elem = NULL;

    snprintf(elemProp, sizeof(elemProp), "%s_enabled", name);
    configGetInt(themeConfig, elemProp, &enabled);

    if (enabled) {
        snprintf(elemProp, sizeof(elemProp), "%s_type", name);
        configGetStr(themeConfig, elemProp, &type);
        if (type) {
            if (!strcmp(elementsType[ELEM_TYPE_ATTRIBUTE_TEXT], type)) {
                elems->needsItemConfig = 1;
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_ATTRIBUTE_TEXT, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                initAttributeText(themePath, themeConfig, theme, elem, name);
            } else if (!strcmp(elementsType[ELEM_TYPE_STATIC_TEXT], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_STATIC_TEXT, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                initStaticText(themePath, themeConfig, theme, elem, name);
            } else if (!strcmp(elementsType[ELEM_TYPE_GAME_COUNT_TEXT], type)) {
                elems->needsItemConfig = 1;
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_STATIC_TEXT, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                initGameCountText(themePath, themeConfig, theme, elem, name);
            } else if (!strcmp(elementsType[ELEM_TYPE_ATTRIBUTE_IMAGE], type)) {
                elems->needsItemConfig = 1;
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_ATTRIBUTE_IMAGE, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, gDefaultCol, theme->fonts[0]);
                initAttributeImage(themePath, themeConfig, theme, elem, name);
            } else if (!strcmp(elementsType[ELEM_TYPE_GAME_IMAGE], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_GAME_IMAGE, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, gDefaultCol, theme->fonts[0]);
                initGameImage(themePath, themeConfig, theme, elem, name, NULL, 1, NULL, NULL);
            } else if (!strcmp(elementsType[ELEM_TYPE_STATIC_IMAGE], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_STATIC_IMAGE, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, gDefaultCol, theme->fonts[0]);
                initStaticImage(themePath, themeConfig, theme, elem, name, NULL);
            } else if (!strcmp(elementsType[ELEM_TYPE_BACKGROUND], type)) {
                if (!elems->first) { // Background elem can only be the first one
                    elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_BACKGROUND, 0, 0, ALIGN_NONE, screenWidth, screenHeight, SCALING_NONE, gDefaultCol, theme->fonts[0]);
                    initBackground(themePath, themeConfig, theme, elem, name, NULL, 1, NULL);
                }
            } else if (!strcmp(elementsType[ELEM_TYPE_MENU_ICON], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_MENU_ICON, screenWidth >> 1, 400, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, gDefaultCol, theme->fonts[0]);
                elem->drawElem = &drawMenuIcon;
            } else if (!strcmp(elementsType[ELEM_TYPE_MENU_TEXT], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_MENU_TEXT, screenWidth >> 1, 20, ALIGN_CENTER, 200, 20, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                elem->drawElem = &drawMenuText;
            } else if (!strcmp(elementsType[ELEM_TYPE_ITEMS_LIST], type)) {
                // A devices=-filtered ItemsList is a per-device OVERRIDE: it joins its family's
                // draw list but must NOT claim one of the four global nav slots below (games/apps/
                // favs/vcd are claimed in strict parse order; a filtered element in that chain
                // would corrupt the family-to-slot mapping and could silently drop later lists).
                // Navigation resolves it per page via thmResolveItemsList instead.
                char devProp[64];
                const char *devValue;
                snprintf(devProp, sizeof(devProp), "%s_devices", name);
                if (configGetStr(themeConfig, devProp, &devValue) && thmParseDeviceList(devValue, 1 /* initBasic re-parses and logs */) != 0) {
                    elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_ITEMS_LIST, 42, 42, ALIGN_NONE, 400, 360, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                    initItemsList(themePath, themeConfig, theme, elem, name, NULL);
                    // Pre-existing quirk kept as-is: an UNFILTERED ItemsList parsed for an INFO
                    // family still claims the next nav slot below (thmLoad's info loops run after
                    // the main ones). Refusing info-family claims here would change behavior for
                    // themes that rely on it.
                } else if (!theme->gamesItemsList) {
                    elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_ITEMS_LIST, 0, 0, ALIGN_NONE, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                    initItemsList(themePath, themeConfig, theme, elem, name, NULL);
                    theme->gamesItemsList = elem;
                } else if (!theme->appsItemsList) {
                    elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_ITEMS_LIST, 42, 42, ALIGN_NONE, 400, 360, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                    initItemsList(themePath, themeConfig, theme, elem, name, NULL);
                    theme->appsItemsList = elem;
                } else if (!theme->favsItemsList) {
                    elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_ITEMS_LIST, 42, 42, ALIGN_NONE, 400, 360, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                    initItemsList(themePath, themeConfig, theme, elem, name, NULL);
                    theme->favsItemsList = elem;
                } else if (!theme->vcdItemsList) {
                    elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_ITEMS_LIST, 42, 42, ALIGN_NONE, 400, 360, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                    initItemsList(themePath, themeConfig, theme, elem, name, NULL);
                    theme->vcdItemsList = elem; // 4th slot: the L3 VCD view's own items list (own cover cache)
                }
            } else if (!strcmp(elementsType[ELEM_TYPE_ITEM_ICON], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_GAME_IMAGE, 0, 0, ALIGN_CENTER, 64, 64, SCALING_RATIO, gDefaultCol, theme->fonts[0]);
                initGameImage(themePath, themeConfig, theme, elem, name, "ICO", 20, NULL, NULL);
            } else if (!strcmp(elementsType[ELEM_TYPE_ITEM_COVER], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_GAME_IMAGE, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, gDefaultCol, theme->fonts[0]);
                initGameImage(themePath, themeConfig, theme, elem, name, "COV", 10, NULL, NULL);
            } else if (!strcmp(elementsType[ELEM_TYPE_ITEM_TEXT], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_ITEM_TEXT, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                elem->drawElem = &drawItemText;
            } else if (!strcmp(elementsType[ELEM_TYPE_HINT_TEXT], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_HINT_TEXT, 16, -HINT_HEIGHT, ALIGN_NONE, 12, 20, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                elem->drawElem = &drawHintText;
            } else if (!strcmp(elementsType[ELEM_TYPE_INFO_HINT_TEXT], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_INFO_HINT_TEXT, 16, -HINT_HEIGHT, ALIGN_NONE, 12, 20, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                elem->drawElem = &drawInfoHintText;
            } else if (!strcmp(elementsType[ELEM_TYPE_LOADING_ICON], type)) {
                if (!theme->loadingIcon)
                    theme->loadingIcon = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_LOADING_ICON, -40, -60, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, gDefaultCol, theme->fonts[0]);
            } else if (!strcmp(elementsType[ELEM_TYPE_BDM_INDEX], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_BDM_INDEX, screenWidth >> 1, 355, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, gDefaultCol, theme->fonts[0]);
                elem->drawElem = &drawBDMIndex;
            } else if (!strcmp(elementsType[ELEM_TYPE_COVERFLOW], type)) {
                // GAME_IMAGE-backed (COV cache) so initMutableImage/findDuplicate work unchanged.
                // Allow one per list (e.g. a games "main" coverflow + an "appsMain" coverflow,
                // like wOPL); initCoverflow points gTheme->coverflow at the FIRST as the active flag.
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_GAME_IMAGE, screenWidth >> 1, screenHeight >> 1, ALIGN_CENTER, 150, 210, SCALING_RATIO, gDefaultCol, theme->fonts[0]);
                initCoverflow(themePath, themeConfig, theme, elem, name);
            }

            if (elem) {
                if (!elems->first)
                    elems->first = elem;

                if (!elems->last)
                    elems->last = elem;
                else {
                    elems->last->next = elem;
                    elems->last = elem;
                }
            }
        } else
            return 0; // ends the reading of elements
    }

    return 1;
}

static void freeGUIElems(theme_elems_t *elems)
{
    theme_element_t *elem = elems->first;
    while (elem) {
        elems->first = elem->next;
        elem->endElem(elem);
        elem = elems->first;
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

GSTEXTURE *thmGetTexture(unsigned int id)
{
    if (id >= TEXTURES_COUNT)
        return NULL;
    else {
        // see if the texture is valid
        GSTEXTURE *txt = &gTheme->textures[id];

        if (txt->Mem)
            return txt;
        else
            return NULL;
    }
}

static void thmFree(theme_t *theme)
{
    if (theme) {
        // free elements
        freeGUIElems(&theme->mainElems);
        freeGUIElems(&theme->infoElems);
        freeGUIElems(&theme->appsMainElems);
        freeGUIElems(&theme->appsInfoElems);
        freeGUIElems(&theme->favsMainElems);
        freeGUIElems(&theme->favsInfoElems);
        freeGUIElems(&theme->vcdMainElems);
        freeGUIElems(&theme->vcdInfoElems);

        // free textures
        GSTEXTURE *texture;
        int id = 0;
        for (; id < TEXTURES_COUNT; id++) {
            texture = &theme->textures[id];
            if (texture->Mem != NULL) {
                rmUnloadTexture(texture);
                texFree(texture);
            }
        }

        // free fonts
        for (id = 0; id < THM_MAX_FONTS; ++id)
            fntRelease(theme->fonts[id]);

        // The loading icon is stored outside the four managed element lists, so
        // free it explicitly (endBasic) to avoid leaking it on every theme reload.
        if (theme->loadingIcon)
            theme->loadingIcon->endElem(theme->loadingIcon);

        free(theme);
    }
}

static int thmReadEntry(int index, const char *path, const char *separator, const char *name, unsigned char d_type)
{
    // #152: don't trust d_type alone. It is derived from iomanX FIO_S_IFDIR mode bits, but MMCE
    // card FIRMWARE fills stat.mode verbatim over the wire (mmceman copies it through), and clone
    // firmwares (e.g. Bitfunx PSxMemCard) may not speak the FIO_S_* dialect -- every entry then
    // degrades to DT_UNKNOWN. That is why languages still listed (lngReadEntry accepts != DT_DIR)
    // while themes vanished (this required == DT_DIR exactly). For a thm_-prefixed candidate that
    // is not positively DT_DIR, confirm with an opendir() probe: it succeeds only for directories
    // on every driver regardless of mode-bit dialect, and runs only for THM-folder candidates.
    int isDir = (d_type == DT_DIR);
    if (!isDir && strncmp(name, "thm_", 4) == 0) {
        char probe[256];
        snprintf(probe, sizeof(probe), "%s%s%s", path, separator, name);
        DIR *pd = opendir(probe);
        if (pd != NULL) {
            closedir(pd);
            isDir = 1;
        }
    }

    // strncmp, not upstream's strstr: the name parse below assumes the prefix is at the START
    // (name + 4), so a mid-string "thm_" match always produced a garbage theme name anyway.
    if (isDir && strncmp(name, "thm_", 4) == 0) {
        theme_file_t *currTheme = &themes[nThemes + index];

        int length = strlen(name) - 4 + 1;
        currTheme->name = (char *)malloc(length * sizeof(char));
        memcpy(currTheme->name, name + 4, length);
        currTheme->name[length - 1] = '\0';

        length = strlen(path) + 1 + strlen(name) + 1 + 1;
        currTheme->filePath = (char *)malloc(length * sizeof(char));
        sprintf(currTheme->filePath, "%s%s%s%s", path, separator, name, separator);

        LOG("THEMES Theme found: %s\n", currTheme->filePath);

        index++;
    }
    return index;
}

/* themePath must contains the leading separator (as it is dependent of the device, we can't know here) */
static int thmLoadResource(GSTEXTURE *texture, int texId, const char *themePath, short psm, int useDefault)
{
    int success = -1;

    if (themePath != NULL)
        success = texDiscoverLoad(texture, themePath, texId); // only set success here

    if ((success < 0) && useDefault)
        texLoadInternal(texture, texId); // we don't mind the result of "default"

    return success;
}

static void thmSetColors(theme_t *theme)
{
    memcpy(theme->bgColor, gDefaultBgColor, 3);
    memcpy(theme->plasBlendColor, gDefaultPlasBlendColor, 3);
    theme->textColor = GS_SETREG_RGBA(gDefaultTextColor[0], gDefaultTextColor[1], gDefaultTextColor[2], 0x80);
    theme->uiTextColor = GS_SETREG_RGBA(gDefaultUITextColor[0], gDefaultUITextColor[1], gDefaultUITextColor[2], 0x80);
    theme->selTextColor = GS_SETREG_RGBA(gDefaultSelTextColor[0], gDefaultSelTextColor[1], gDefaultSelTextColor[2], 0x80);

    theme_element_t *elem = theme->mainElems.first;
    while (elem) {
        elem->color = theme->textColor;
        elem = elem->next;
    }
}

static void thmLoadFonts(config_set_t *themeConfig, const char *themePath, theme_t *theme)
{
    int fntID; // theme side font id, not the fntSys handle
    for (fntID = 0; fntID < THM_MAX_FONTS; ++fntID) {
        // does the font by the key exist?
        char fntKey[16];

        if (fntID == 0) {
            snprintf(fntKey, sizeof(fntKey), "default_font");
            theme->fonts[0] = FNT_DEFAULT;
        } else {
            snprintf(fntKey, sizeof(fntKey), "font%d", fntID);
            theme->fonts[fntID] = theme->fonts[0];
        }

        char fullPath[128];
        const char *fntFile;
        if (configGetStr(themeConfig, fntKey, &fntFile)) {
            snprintf(fullPath, sizeof(fullPath), "%s%s", themePath, fntFile);

            int fontSize;
            char sizeKey[64];
            if (fntID == 0)
                snprintf(sizeKey, sizeof(sizeKey), "default_font_size");
            else
                snprintf(sizeKey, sizeof(sizeKey), "font%d_size", fntID);

            if (!configGetInt(themeConfig, sizeKey, &fontSize) || fontSize <= 0)
                fontSize = FNTSYS_DEFAULT_SIZE;

            int fntHandle = fntLoadFile(fullPath, fontSize);
            // Do we have a valid font? Assign the font handle to the theme font slot
            if (fntHandle != FNT_ERROR)
                theme->fonts[fntID] = fntHandle;
        }
    }
}

// #120: how long a theme (re)load waits for MMCE-backed art to abort before giving up (mirrors
// mmcesupport.c's MMCE_ART_ABORT_WAIT_TICKS). A wedged MMCE read that never returns makes thmLoad
// ABANDON the swap and keep the current theme (never cacheEnd(1): its TerminateThread would kill the
// worker mid-fileXio and orphan the shared RPC channel), so a theme swap can never hang.
#define THM_MMCE_ART_ABORT_WAIT_TICKS 500

// Returns 0 on success, -1 when the load was ABANDONED because the art worker would not drain (a
// wedged MMCE read holding the shared fileXio channel). On -1 the current theme stays untouched;
// callers must not commit the new theme id, so a later trigger (another device's theme scan, a
// settings re-apply) naturally retries once the card recovers.
static int thmLoad(const char *themePath)
{
    LOG("THEMES Load theme path=%s\n", themePath);
    char path[256];
    theme_t *curT = gTheme;
    theme_t *newT = (theme_t *)malloc(sizeof(theme_t));
    memset(newT, 0, sizeof(theme_t));

    newT->useDefault = 1;
    newT->usedHeight = 480;
    thmSetColors(newT);
    newT->mainElems.first = NULL;
    newT->mainElems.last = NULL;
    newT->infoElems.first = NULL;
    newT->infoElems.last = NULL;
    newT->appsMainElems.first = NULL;
    newT->appsMainElems.last = NULL;
    newT->appsInfoElems.first = NULL;
    newT->appsInfoElems.last = NULL;
    newT->favsMainElems.first = NULL;
    newT->favsMainElems.last = NULL;
    newT->favsInfoElems.first = NULL;
    newT->favsInfoElems.last = NULL;
    newT->vcdMainElems.first = NULL;
    newT->vcdMainElems.last = NULL;
    newT->vcdInfoElems.first = NULL;
    newT->vcdInfoElems.last = NULL;
    newT->gameCacheCount = 0;
    newT->itemsList = NULL;
    newT->gamesItemsList = NULL;
    newT->appsItemsList = NULL;
    newT->favsItemsList = NULL;
    newT->vcdItemsList = NULL;
    newT->coverflow = NULL;
    newT->coverflowCoverOffset = 0;
    newT->loadingIcon = NULL;
    newT->loadingIconCount = LOAD7_ICON - LOAD0_ICON + 1;

    // #120: a wedged MMCE art read (SD2PSX / MemCard PRO2 card still mid-mount at boot or right after an
    // IGR return) holds the single shared fileXio channel; this theme (re)load's own blocking reads
    // below -- and the pre-swap art drain at the end -- would then wait on it forever, freezing the
    // whole screen (game list stuck at 0,0) and needing a power cycle. Abort MMCE-backed art with a
    // timeout FIRST so the channel is free before we touch storage; if the worker will NOT drain (a
    // truly wedged read), BAIL OUT: keep the current theme and let a later trigger retry. NEVER force-
    // reset here (cacheEnd(1)): its TerminateThread would kill the worker mid-fileXio and orphan the
    // SHARED RPC channel while OPL keeps running, hanging every later fileXio user -- including this
    // function's own reads. (mmceLaunchGame can afford that reset only because it execs away right
    // after.) The very first load (curT == NULL) never bails: pre-cacheInit there are no requests, so
    // the abort returns success immediately.
    if (!cacheAbortMmceImageLoadsTimed(THM_MMCE_ART_ABORT_WAIT_TICKS) && curT != NULL) {
        LOG("THEMES Load: MMCE art worker wedged -- keeping the current theme (retry later)\n");
        free(newT);
        return -1;
    }

    config_set_t *themeConfig = NULL;
    if (!themePath) {
        // No theme specified. Prepare and load the default theme.
        themeConfig = configAlloc(0, NULL, NULL);
        if (gLoadCoverflowBuiltin)
            configReadBuffer(themeConfig, &theme_coverflow_cfg, size_theme_coverflow_cfg);
        else
            configReadBuffer(themeConfig, &conf_theme_OPL_cfg, size_conf_theme_OPL_cfg);
    } else {
        snprintf(path, sizeof(path), "%sconf_theme.cfg", themePath);
        themeConfig = configAlloc(0, NULL, path);
        configRead(themeConfig); // try to load the theme config file. If it does not exist, defaults will be used.
    }

    int intValue;
    if (configGetInt(themeConfig, "use_default", &intValue))
        newT->useDefault = intValue;

    if (configGetInt(themeConfig, "use_real_height", &intValue)) {
        if (intValue)
            newT->usedHeight = screenHeight;
    }

    configGetColor(themeConfig, "bg_color", newT->bgColor);
    // absent key leaves the thmSetColors default (settings picker value; black out of the box)
    configGetColor(themeConfig, CONFIG_OPL_PLAS_BLEND_COLOR, newT->plasBlendColor);

    unsigned char color[3];
    if (configGetColor(themeConfig, "text_color", color))
        newT->textColor = GS_SETREG_RGBA(color[0], color[1], color[2], 0x80);

    if (configGetColor(themeConfig, "ui_text_color", color))
        newT->uiTextColor = GS_SETREG_RGBA(color[0], color[1], color[2], 0x80);

    if (configGetColor(themeConfig, "sel_text_color", color))
        newT->selTextColor = GS_SETREG_RGBA(color[0], color[1], color[2], 0x80);

    if (configGetInt(themeConfig, "coverflow_cover_offset", &intValue)) {
        // clamp -- this feeds coverWidth * offset / 256 in drawCoverFlow; an
        // unbounded value from an untrusted theme .cfg would signed-overflow that
        // multiply (Codex audit, Low 1). The range is far wider than any real theme.
        if (intValue < -1024)
            intValue = -1024;
        else if (intValue > 1024)
            intValue = 1024;
        newT->coverflowCoverOffset = intValue;
    }

    // before loading the element definitions, we have to have the fonts prepared
    // for that, we load the fonts and a translation table
    if (themePath)
        thmLoadFonts(themeConfig, themePath, newT);

    int i = 1, j;
    snprintf(path, sizeof(path), "main0");
    while (addGUIElem(themePath, themeConfig, newT, &newT->mainElems, NULL, path))
        snprintf(path, sizeof(path), "main%d", i++);

    for (j = 0; j < i; j++) {
        snprintf(path, sizeof(path), "appsMain%d", j);

        if (addGUIElem(themePath, themeConfig, newT, &newT->appsMainElems, NULL, path))
            continue;
        else {
            snprintf(path, sizeof(path), "main%d", j);
            addGUIElem(themePath, themeConfig, newT, &newT->appsMainElems, NULL, path);
        }
    }

    // Favourites family: favsMain<j> override, else fall back to main<j> (identical to appsMain).
    // Runs after appsMain so a favsMain ItemsList claims the 3rd slot (favsItemsList), and before
    // the info passes so an info ItemsList never steals it.
    for (j = 0; j < i; j++) {
        snprintf(path, sizeof(path), "favsMain%d", j);

        if (addGUIElem(themePath, themeConfig, newT, &newT->favsMainElems, NULL, path))
            continue;
        else {
            snprintf(path, sizeof(path), "main%d", j);
            addGUIElem(themePath, themeConfig, newT, &newT->favsMainElems, NULL, path);
        }
    }

    // VCD/PS1 view main family: vcdMain<j> override, else fall back to appsMain<j> (the square box),
    // else main<j>. The apps fallback means a theme that defines no vcdMain still gets the square VCD
    // look it had before this family existed. An ItemsList reached via this fallback claims the 4th
    // slot (vcdItemsList) in addGUIElem, giving the VCD list its OWN cover cache (separateVcdCoverCache).
    for (j = 0; j < i; j++) {
        snprintf(path, sizeof(path), "vcdMain%d", j);
        if (addGUIElem(themePath, themeConfig, newT, &newT->vcdMainElems, NULL, path))
            continue;
        snprintf(path, sizeof(path), "appsMain%d", j);
        if (addGUIElem(themePath, themeConfig, newT, &newT->vcdMainElems, NULL, path))
            continue;
        snprintf(path, sizeof(path), "main%d", j);
        addGUIElem(themePath, themeConfig, newT, &newT->vcdMainElems, NULL, path);
    }

    i = 1;
    snprintf(path, sizeof(path), "info0");
    while (addGUIElem(themePath, themeConfig, newT, &newT->infoElems, NULL, path))
        snprintf(path, sizeof(path), "info%d", i++);

    for (j = 0; j < i; j++) {
        snprintf(path, sizeof(path), "appsInfo%d", j);

        if (addGUIElem(themePath, themeConfig, newT, &newT->appsInfoElems, NULL, path))
            continue;
        else {
            snprintf(path, sizeof(path), "info%d", j);
            addGUIElem(themePath, themeConfig, newT, &newT->appsInfoElems, NULL, path);
        }
    }

    // Favourites info family: favsInfo<j> override, else info<j> (identical to appsInfo).
    for (j = 0; j < i; j++) {
        snprintf(path, sizeof(path), "favsInfo%d", j);

        if (addGUIElem(themePath, themeConfig, newT, &newT->favsInfoElems, NULL, path))
            continue;
        else {
            snprintf(path, sizeof(path), "info%d", j);
            addGUIElem(themePath, themeConfig, newT, &newT->favsInfoElems, NULL, path);
        }
    }

    // VCD/PS1 view info family: vcdInfo<j> override, else fall back to info<j>.
    for (j = 0; j < i; j++) {
        snprintf(path, sizeof(path), "vcdInfo%d", j);

        if (addGUIElem(themePath, themeConfig, newT, &newT->vcdInfoElems, NULL, path))
            continue;
        else {
            snprintf(path, sizeof(path), "info%d", j);
            addGUIElem(themePath, themeConfig, newT, &newT->vcdInfoElems, NULL, path);
        }
    }

    validateGUIElems(themePath, themeConfig, newT);

    newT->itemsList = newT->gamesItemsList;

    // NOTE: themeConfig is freed AFTER the texture-load section below -- the use_settings_bg lookup near
    // the end still reads it. Freeing here was a use-after-free (configGetInt on freed memory).
    LOG("THEMES Number of cache: %d\n", newT->gameCacheCount);
    LOG("THEMES Used height: %d\n", newT->usedHeight);

    // default all to not loaded...
    for (i = 0; i < TEXTURES_COUNT; i++)
        newT->textures[i].Mem = NULL;

    // LOGO, loaded here to avoid flickering during startup with device in AUTO + theme set
    texLoadInternal(&newT->textures[LOGO_PICTURE], LOGO_PICTURE);

    // Optional animated boot-logo frames (embedded build assets logo0..logo6).
    // Count the contiguous frames that actually load; guiRenderGreeting() cycles
    // them on the boot splash. Zero when none are embedded -> single LOGO_PICTURE.
    newT->logoFrameCount = 0;
    for (i = LOGO0_PICTURE; i <= LOGO6_PICTURE; i++) {
        texLoadInternal(&newT->textures[i], i);
        if (newT->textures[i].Mem != NULL)
            newT->logoFrameCount++;
        else
            break;
    }

    // Busy-icon animation frames, each frame probed INDEPENDENTLY: the theme's own load<N>.png
    // first (disk themes), the baked frame only when use_default=1 opts into it. thmLoadResource's
    // return is the DISK load's result only (it discards the embedded fallback's result), so a
    // frame counts as loaded only when its texture slot actually got data. loadingIconCount is the
    // number of CONTIGUOUS frames from load0 -- a gap (e.g. load2.png missing on a use_default=0
    // theme) ends the count instead of letting guiDrawBusy's `LOAD0_ICON + frame % count` wander
    // into texture IDs that hold no frame. The old loop latched themePath_temp=NULL off the FIRST
    // failed custom probe (so one missing/unreadable load0.png silently replaced the theme's whole
    // animation with the baked set -- or, with use_default=0, dropped frames that did exist), which
    // is why custom frames appeared "randomly" (#213).
    newT->loadingIconCount = 0;
    for (i = LOAD0_ICON; i <= LOAD7_ICON; i++) {
        thmLoadResource(&newT->textures[i], i, themePath, GS_PSM_CT32, newT->useDefault);
        if (newT->textures[i].Mem != NULL)
            newT->loadingIconCount++;
        else
            break;
    }

    // Customizable icons
    for (i = BDM_ICON; i <= START_ICON; i++)
        thmLoadResource(&newT->textures[i], i, themePath, GS_PSM_CT32, newT->useDefault);

    // UDPFS_ICON is appended at the very END of the enum (after CASE_OVERLAY2) so that saved
    // favourite icon_ids stay ABI-stable -- which puts it OUTSIDE the BDM_ICON..START_ICON device
    // range above. Load it explicitly with the same disk-override + embedded-default semantics, or
    // the UDPFS filesystem tab and the UDPFSBD block tab (bdmGetIconId returns UDPFS_ICON when
    // gNetBootProtocol == NET_BOOT_UDPFS) draw no icon (thmGetTexture(UDPFS_ICON) returns NULL).
    thmLoadResource(&newT->textures[UDPFS_ICON], UDPFS_ICON, themePath, GS_PSM_CT32, newT->useDefault);

    // Control-hint glyphs + Favourites tab icon/star (contiguous L3_ICON..FAV_MARK: the VCD L3 hint,
    // the Favourites R3 hint, the FAV tab icon FAV_ICON/"fav", the favourited-item star
    // FAV_MARK/"fav_mark"). Theme-overridable with embedded fallback -- the SAME disk-override +
    // baked-default semantics as the BDM_ICON..START_ICON loop above: a disk theme's fav.png /
    // fav_mark.png / L3.png / R3.png win; the baked glyphs fill only what the theme does not ship
    // (and nothing when use_default=0). This loop previously passed themePath=NULL + useDefault=1,
    // so a disk theme's fav.png was never even probed and the baked icon always drew (#213).
    // thmLoadResource is synchronous here, so the baked glyph can never flash ahead of the theme's
    // own file.
    for (i = L3_ICON; i <= FAV_MARK; i++)
        thmLoadResource(&newT->textures[i], i, themePath, GS_PSM_CT32, newT->useDefault);

    // Embedded attribute glyphs (#Format/#Media/Aspect/Rating/Scan/Vmode values): BUILT-IN theme
    // ONLY. a1ae5e5e decoded these for every theme so badges could fall back to them on disk themes
    // -- but the fallback fires on every frame BEFORE the theme's own glyph PNG has loaded off the
    // single art worker, so custom themes FLASHED our baked glyph and then swapped to their own
    // asset (brenotomaz, #213: "the original assets load first"). Doctrine (NathanNeurotic): a
    // custom theme uses its OWN assets entirely -- never the baked set; no glyph shipped means the
    // element's own _default (or nothing), not our art. Gating the decode also reclaims the ~153 KB
    // of EE RAM on disk themes. The draw-site fallback is additionally gated on the built-in
    // (NULL-prefix) cache in drawAttributeImage, so this can never regress by re-widening one side.
    if (!themePath)
        for (i = ELF_FORMAT; i <= VMODE_PAL; i++)
            thmLoadResource(&newT->textures[i], i, NULL, GS_PSM_CT32, 1);

    // Optional settings/menu background (guiDrawBGSettings draws it instead of the plasma).
    // Theme-supplied only for now: a disk theme opts in with use_settings_bg=1 and ships its
    // own settings_bg.png. No embedded default yet (internalDefault[SETTINGS_BG] is NULL), so
    // the built-in <OPL>/<Coverflow> themes leave the slot empty and keep the plasma.
    if (themePath) {
        if (configGetInt(themeConfig, "use_settings_bg", &intValue) && intValue)
            thmLoadResource(&newT->textures[SETTINGS_BG], SETTINGS_BG, themePath, GS_PSM_CT32, 0);
    }

    configFree(themeConfig); // all themeConfig reads are done now (last was use_settings_bg above)

    // #120: bound the pre-swap art drain too (the UNBOUNDED cacheCancelPendingImageLoads() here was the
    // original hard-freeze site). Abort MMCE art first so the drain normally waits only on fast local
    // (USB / MC / VCD) reads; if either step times out on a wedged worker, BAIL: free the fully-built
    // newT -- it was never rendered, so its caches have no outstanding requests and thmFree returns
    // instantly -- and keep rendering the current theme. No force-reset, for the same RPC-orphaning
    // reason as the top abort; and thmFree(curT) must never run while an in-flight request may still
    // reference the old theme's caches (use-after-free), which the successful timed drain rules out.
    if (curT != NULL &&
        (!cacheAbortMmceImageLoadsTimed(THM_MMCE_ART_ABORT_WAIT_TICKS) ||
         !cacheCancelPendingImageLoadsTimed(THM_MMCE_ART_ABORT_WAIT_TICKS))) {
        LOG("THEMES Load: art drain timed out -- keeping the current theme (retry later)\n");
        thmFree(newT);
        return -1;
    }
    if (curT == NULL)
        cacheCancelPendingImageLoads(); // first load: pre-cacheInit, nothing queued -- plain no-op drain
    gTheme = newT;
    thmFree(curT);
    return 0;
}

// Callers MUST hold guiLock (except pre-GUI single-threaded init, where the lock is a not-ready
// no-op): this free()s the list the UI-Settings dialog may be handed via thmGetGuiList, and it
// runs on the IO worker for device-triggered rebuilds. guiShowUIConfig snapshots the list under
// the same lock. NOT self-locking: thmReinit must hold the lock across its name-string frees AND
// this rebuild as ONE critical section (between the two, the published list points at freed
// strings), and the sema is not recursive.
static void thmRebuildGuiNames(void)
{
    if (guiThemesNames)
        free(guiThemesNames);

    // build the themes name list (+1 default internal, +1 built-in coverflow, +1 NULL)
    guiThemesNames = (const char **)malloc((nThemes + 3) * sizeof(char **));

    // add default internal
    guiThemesNames[0] = "<OPL>";

    int i = 0;
    for (; i < nThemes; i++) {
        guiThemesNames[i + 1] = themes[i].name;
    }

    // built-in coverflow theme occupies the slot right after the disk themes
    guiThemesNames[nThemes + 1] = "<Coverflow>";
    guiThemesNames[nThemes + 2] = NULL;
}

int thmAddElements(char *path, const char *separator, int forceRefresh)
{
    int result, i;

    result = listDir(path, separator, THM_MAX_FILES - nThemes, &thmReadEntry);
    nThemes += result;
    // Appends above only fill NEW themes[] slots (the published name list references old slots
    // untouched); the rebuild's free+swap is what must be serialized against the GUI's readers.
    guiLock();
    thmRebuildGuiNames();
    guiUnlock();

    const char *temp;
    if (configGetStr(configGetByType(CONFIG_OPL), "theme", &temp)) {
        LOG("THEMES Trying to set again theme: %s\n", temp);
        if (thmSetGuiValue(thmFindGuiID(temp), 0) && forceRefresh) {
            for (i = 0; i < MODE_COUNT; i++)
                moduleUpdateMenu(i, 1, 0);
        }
    }

    return result;
}

void thmInit(void)
{
    LOG("THEMES Init\n");
    gTheme = NULL;

    thmReloadScreenExtents();

    // initialize default internal
    thmLoad(NULL);

    thmAddElements(gBaseMCDir, "/", 0);
}

void thmReinit(const char *path)
{
    // Nad #5 (settings change reverts the look): this used to UNCONDITIONALLY thmLoad(NULL) +
    // guiThemeID=0 -- reverting to the default <OPL> look even when the ACTIVE theme does not live
    // on the device being removed. Worse, an SMB/ETH re-init calls in here on EVERY settings-OK
    // while the network is in any error state (server asleep, cable out, or plain share-browse
    // mode, which parks in an "error" state even when healthy), and a subsequent Save Changes then
    // wrote the transient "<OPL>" over the user's saved theme name -- reverting it PERMANENTLY.
    // Only fall back to the default when the active theme actually lives on the removed device;
    // otherwise keep it and just re-index (the removal loop reshuffles ids, so re-find by name).
    char activeName[64] = "";
    int activeOnDevice = 0;
    if (guiThemeID >= 1 && guiThemeID <= nThemes) {
        snprintf(activeName, sizeof(activeName), "%s", themes[guiThemeID - 1].name);
        activeOnDevice = strncmp(themes[guiThemeID - 1].filePath, path, strlen(path)) == 0;
    } else if (guiThemeID == nThemes + 1) {
        snprintf(activeName, sizeof(activeName), "<Coverflow>"); // built-in; its id (nThemes+1) shifts as themes are removed
    }

    // One guiLock section across the removal loop AND the rebuild: the frees below invalidate
    // strings the CURRENTLY PUBLISHED guiThemesNames points at, so the GUI-side readers
    // (frame-locked renders, guiShowUIConfig's snapshot) must not run between free and swap.
    guiLock();
    int i = 0;
    while (i < nThemes) {
        if (strncmp(themes[i].filePath, path, strlen(path)) == 0) {
            LOG("THEMES Remove theme: %s\n", themes[i].filePath);
            nThemes--;
            free(themes[i].name);
            themes[i].name = themes[nThemes].name;
            themes[nThemes].name = NULL;
            free(themes[i].filePath);
            themes[i].filePath = themes[nThemes].filePath;
            themes[nThemes].filePath = NULL;
        } else
            i++;
    }

    thmRebuildGuiNames();
    guiUnlock();

    if (activeOnDevice) {
        // The displayed theme's files just went away with its device: drop to the built-in default.
        // Honor thmLoad's abandon-and-retry (#120): commit the default id only if the swap happened,
        // else keep the old id so the still-displayed theme and the saved config stay consistent.
        if (thmLoad(NULL) == 0)
            guiThemeID = 0;
    } else if (guiThemeID != 0) {
        // Active theme untouched (built-in coverflow, or a theme on another device): keep it. gTheme
        // needs no reload -- its loaded resources are unaffected by the removed entries.
        guiThemeID = thmFindGuiID(activeName);
    }
}

void thmReloadScreenExtents(void)
{
    rmGetScreenExtents(&screenWidth, &screenHeight);
}

const char *thmGetValue(void)
{
    return guiThemesNames[guiThemeID];
}

int thmSetGuiValue(int themeID, int reload)
{
    if (themeID != -1) {
        if (guiThemeID != themeID || reload) {
            int loadResult;
            if (themeID == nThemes + 1) {
                // built-in coverflow theme: load the embedded buffer via thmLoad(NULL).
                // Checked BEFORE the themes[themeID - 1] access below (which would be OOB).
                gLoadCoverflowBuiltin = 1;
                loadResult = thmLoad(NULL);
                gLoadCoverflowBuiltin = 0;
            } else
                loadResult = thmLoad(themeID != 0 ? themes[themeID - 1].filePath : NULL);

            // #120: an abandoned load (wedged MMCE art worker) keeps the CURRENT theme -- do not
            // commit the new id, so the next thmAddElements/settings-apply trigger retries it.
            if (loadResult != 0)
                return 0;

            guiThemeID = themeID;
            return 1;
        } else if (guiThemeID == 0)
            thmSetColors(gTheme);
    }
    return 0;
}

int thmGetGuiValue(void)
{
    return guiThemeID;
}

int thmFindGuiID(const char *theme)
{
    if (theme) {
        if (strcasecmp(theme, "<Coverflow>") == 0)
            return nThemes + 1; // built-in coverflow theme
        int i = 0;
        for (; i < nThemes; i++) {
            if (strcasecmp(themes[i].name, theme) == 0)
                return i + 1;
        }
    }
    return 0;
}

const char **thmGetGuiList(void)
{
    return guiThemesNames;
}

char *thmGetFilePath(int themeID)
{
    // Disk themes occupy IDs 1..nThemes. The built-ins (<OPL> = 0, <Coverflow> = nThemes+1)
    // and any out-of-range ID have no on-disk path -> return NULL so callers fall back to
    // the default/internal assets instead of indexing themes[] out of bounds.
    if (themeID <= 0 || themeID > nThemes)
        return NULL;

    theme_file_t *currTheme = &themes[themeID - 1];
    char *path = currTheme->filePath;

    return path;
}

void thmEnd(void)
{
    thmFree(gTheme);

    int i = 0;
    for (; i < nThemes; i++) {
        free(themes[i].name);
        free(themes[i].filePath);
    }

    free(guiThemesNames);
}

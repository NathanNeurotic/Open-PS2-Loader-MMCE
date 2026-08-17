/*
  Copyright 2009, Ifcaro & volca
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#include "include/opl.h"
#include "include/diag.h"
#include "include/util.h"
#include "include/ioman.h"
#include "include/sound.h"
#include <string.h>
#include <ctype.h> // isalpha/isalnum -- libconfig identifier grammar in cfgIsLibconfigIdent
#include <errno.h>

// FIXME: We should not need this function.
//        Use newlib's 'stat' to get GMT time.
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h> // iox_stat_t, fileXioGetStat
int configGetStat(config_set_t *configSet, iox_stat_t *stat);

static u32 currentUID = 0;
static config_set_t configFiles[CONFIG_INDEX_COUNT];
static char legacyNetConfigPath[256] = "mc?:SYS-CONF/IPCONFIG.DAT";
static const char *configFilenames[CONFIG_INDEX_COUNT] = {
    CONFIG_OPL_FILENAME, // RiptOPL master settings (settings_riptopl.cfg; legacy conf_riptopl.cfg auto-migrated on read -- see configRead)
    "conf_last.cfg",
    "conf_apps.cfg",
    "conf_network.cfg",
    "conf_game.cfg",
};

static int strToColor(const char *string, unsigned char *color)
{
    int cnt = 0, n = 0;
    color[0] = 0;
    color[1] = 0;
    color[2] = 0;

    if (!string || !*string)
        return 0;
    if (string[0] != '#')
        return 0;

    string++;

    while (*string) {
        int fh = fromHex(*string);
        if (fh >= 0) {
            if (n >= 3)
                break; // never write past the caller's 3-byte color[] (RGB)
            color[n] = color[n] * 16 + fh;
        } else {
            break;
        }

        // Two characters per color
        if (cnt == 1) {
            cnt = 0;
            n++;
        } else {
            cnt++;
        }

        string++;
    }

    return 1;
}

/// true if given a whitespace character
int isWS(char c)
{
    return c == ' ' || c == '\t';
}

// ---- Dual-format support (wOPL/libconfig) ---------------------------------------------------------
// wOPL migrated to libconfig and rewrites SHARED files in place (app title.cfg on every Apps scan) or
// replaces them (conf_theme.cfg -> wopl_theme.cfg). We read BOTH syntaxes and write back whichever we
// read, so we never convert a user's file out from under another program -- and never lose their data.
//
// WHY DETECTION AND NOT "TRY-LEGACY-THEN-FALL-BACK": splitAssignment below does NOT fail on libconfig.
// Fed `title = "My App";` it happily returns key="title " (trailing space) and val=" \"My App\";" --
// and getConfigItemForName does an exact strncmp, so the lookup misses and the app silently vanishes
// from the list. Nothing ever reports an error, so there is no failure to fall back FROM.
//
// THE DISCRIMINATOR IS SYNTAX, NOT KEY NAMES. An earlier draft keyed off our '$'/'#' prefixes; that is
// wrong -- app title.cfg uses bare lowercase `title`/`boot`/`argv1` (appsupport.h), exactly like wOPL,
// and conf_apps.cfg uses the user's own app title as the key. Instead, note that the two WRITERS are
// fully characterised and cannot overlap:
//   ours (configWrite below): "%s=%s\r\n"  -- NEVER a space before '=', NEVER a trailing ';'
//   theirs (libconfig):       "key = val;" -- ALWAYS " = ", ALWAYS a trailing ';'
// Requiring BOTH, plus a legal libconfig identifier, makes misclassification impossible for anything
// either writer can emit.
//
// A ':' NEVER SIGNALS LIBCONFIG. Our own legacy theme files open with a section prefix -- see
// misc/conf_theme_OPL.cfg, whose FIRST line is `main0:` (25 such lines) -- parsed by parsePrefix and
// composed into `main0_type`. Treating `name :` as libconfig-exclusive would misdetect our own shipped
// theme and feed it to the wrong parser. We do not need ':' anyway: both of wOPL's writers emit a
// SCALAR first (`compat = <n>;` for per-game, `title = "...";` for title.cfg), so the very first
// meaningful line always settles the format before any group can appear.
enum {
    CFG_LINE_BLANK,
    CFG_LINE_COMMENT,
    CFG_LINE_LEGACY,
    CFG_LINE_LIBCONFIG
};

// libconfig identifier grammar: [A-Za-z*][-A-Za-z0-9_*]*  . Pure read of [begin,end); no copies.
static int cfgIsLibconfigIdent(const char *begin, const char *end)
{
    const char *p;

    if (begin >= end)
        return 0;
    if (!isalpha((unsigned char)*begin) && *begin != '*')
        return 0;

    for (p = begin + 1; p < end; ++p) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '*')
            return 0;
    }
    return 1;
}

static int cfgClassifyLine(const char *line)
{
    const char *p = line, *end, *eq;

    while (isWS(*p))
        ++p;

    if (*p == '\0')
        return CFG_LINE_BLANK;
    if (*p == '#') // legacy comment AND libconfig comment: tells us nothing either way
        return CFG_LINE_COMMENT;
    if (p[0] == '/' && (p[1] == '/' || p[1] == '*')) // libconfig-only, but still not a key line
        return CFG_LINE_COMMENT;

    end = p + strlen(p);
    while (end > p && isWS(end[-1]))
        --end;

    eq = strchr(p, '=');
    if (eq == NULL || eq >= end)
        return CFG_LINE_LEGACY; // no '=' -> our parser's existing malformed/prefix branch handles it

    // BOTH conditions, and a legal identifier. Our writer can satisfy none of them:
    //   "$Compatibility=0"   -> no space before '=', no ';', '$' is not an identifier char
    //   "title=Foo"          -> no space before '=', no ';'
    //   "title = Foo"        -> hand-edited legacy: space, but no ';'  -> stays LEGACY
    //   "exit_path=a = b;"   -> strchr finds the FIRST '=', which has no space before it
    if (eq > p && isWS(eq[-1]) && end[-1] == ';') {
        const char *kend = eq; // the key is [p, kend) once the padding before '=' is trimmed
        while (kend > p && isWS(kend[-1]))
            --kend;
        if (cfgIsLibconfigIdent(p, kend))
            return CFG_LINE_LIBCONFIG;
    }

    return CFG_LINE_LEGACY;
}

static int splitAssignment(char *line, char *key, size_t keymax, char *val, size_t valmax)
{
    // skip whitespace
    for (; isWS(*line); ++line)
        ;

    // find "=".
    // If found, the text before is key, after is val.
    // Otherwise malformed string is encountered

    char *eqpos = strchr(line, '=');

    if (eqpos) {
        // copy the key and value, reserving room for the NUL terminator so an
        // exactly-buffer-length key/val cannot be left unterminated (OOB read)
        size_t keylen = min(keymax - 1, (size_t)(eqpos - line));

        strncpy(key, line, keylen);
        key[keylen] = '\0';

        eqpos++;

        size_t vallen = min(valmax - 1, (size_t)(strlen(line) - (eqpos - line)));
        strncpy(val, eqpos, vallen);
        val[vallen] = '\0';
    }

    return (eqpos != NULL);
}

static int parsePrefix(char *line, char *prefix, size_t prefixSize)
{
    // find ":".
    // If found, the text before is the prefix.
    // Otherwise a malformed string is encountered.
    char *colpos = strchr(line, ':');

    if (colpos && colpos != line) {
        // copy the prefix, bounded to the destination buffer so a long
        // pre-colon segment in a user-editable config file cannot overflow it
        size_t n = (size_t)(colpos - line);
        if (prefixSize == 0)
            return 0;
        if (n >= prefixSize)
            n = prefixSize - 1;
        memcpy(prefix, line, n);
        prefix[n] = 0;

        return 1;
    } else {
        return 0;
    }
}

static int configKeyValidate(const char *key)
{
    if (strlen(key) == 0)
        return 0;

    return !strchr(key, '=');
}

static struct config_value_t *allocConfigItem(const char *key, const char *val)
{
    struct config_value_t *it = (struct config_value_t *)malloc(sizeof(struct config_value_t));
    if (it == NULL)
        return NULL;
    strncpy(it->key, key, sizeof(it->key));
    it->key[sizeof(it->key) - 1] = '\0';
    strncpy(it->val, val, sizeof(it->val));
    it->val[sizeof(it->val) - 1] = '\0';
    it->next = NULL;

    return it;
}

/// Low level key addition. Does not check for uniqueness.
static void addConfigValue(config_set_t *configSet, const char *key, const char *val)
{
    if (!configSet->tail) {
        configSet->head = allocConfigItem(key, val);
        configSet->tail = configSet->head;
    } else {
        struct config_value_t *it = allocConfigItem(key, val);
        if (it != NULL) {
            configSet->tail->next = it;
            configSet->tail = it;
        }
    }
}

static struct config_value_t *getConfigItemForName(config_set_t *configSet, const char *name)
{
    struct config_value_t *val = configSet->head;

    while (val) {
        if (strncmp(val->key, name, sizeof(val->key)) == 0)
            break;

        val = val->next;
    }

    return val;
}

static char cfgDevice[8];
static char cfgLoadDevice[8];

static void configPrepareDevice(char *device, size_t deviceSize, const char *prefix)
{
    char *colpos;

    if (prefix == NULL)
        prefix = "";
    snprintf(device, deviceSize, "%s", prefix);
    if ((colpos = strchr(device, ':')) != NULL)
        *(colpos + 1) = '\0';
}

char *configGetDir(void)
{
    // Substitute the wildcard slot digit ONLY when it actually is the "mc?:" wildcard AND a card was
    // detected. Blind substitution stamped getmcID()'s -1 (0xFF) -- or a probe-time other-card digit --
    // over a CONCRETE "mc0:"/"mc1:" prefix from an appdir-on-MC boot, so the "Settings saved to %s"
    // toast rendered a garbage byte or the wrong card. Legacy "mc?:" homes keep today's behaviour.
    if (!strncmp(cfgDevice, "mc", 2) && cfgDevice[2] == '?' && getmcID() >= 0)
        cfgDevice[2] = getmcID();

    return cfgDevice;
}

char *configGetLoadDir(void)
{
    if (!strncmp(cfgLoadDevice, "mc", 2) && cfgLoadDevice[2] == '?' && getmcID() >= 0)
        cfgLoadDevice[2] = getmcID();

    return cfgLoadDevice[0] != '\0' ? cfgLoadDevice : cfgDevice;
}

void configPrepareNotifications(char *prefix)
{
    configPrepareDevice(cfgDevice, sizeof(cfgDevice), prefix);
}

static void configPrepareLoadNotification(const char *path)
{
    // pfs0: is OPL's transient mount name. When it backs the known persistent APA home, report
    // the physical HDD owner instead so "loaded from" names the device that actually supplied it.
    if (path != NULL && !strncmp(path, "pfs0:", 5) && gOPLPart[0] != '\0')
        configPrepareDevice(cfgLoadDevice, sizeof(cfgLoadDevice), gOPLPart);
    else
        configPrepareDevice(cfgLoadDevice, sizeof(cfgLoadDevice), path);
}

static void configBuildPath(char *out, size_t outSize, const char *prefix, const char *filename)
{
    size_t len = prefix ? strlen(prefix) : 0;
    if (len == 0) {
        snprintf(out, outSize, "%s", filename);
    } else if (prefix[len - 1] == '/' || prefix[len - 1] == ':') {
        snprintf(out, outSize, "%s%s", prefix, filename);
    } else {
        snprintf(out, outSize, "%s/%s", prefix, filename);
    }
}

void configInit(char *prefix)
{
    char path[256];
    int i;

    if (!prefix)
        prefix = gBaseMCDir;

    configBuildPath(legacyNetConfigPath, sizeof(legacyNetConfigPath), prefix, "IPCONFIG.DAT");

    for (i = 0; i < CONFIG_INDEX_COUNT; i++) {
        configBuildPath(path, sizeof(path), prefix, configFilenames[i]);
        configAlloc(1 << i, &configFiles[i], path);
    }

    configPrepareNotifications(prefix);
}

void configSetMove(char *prefix)
{
    char path[256];
    int i;

    if (!prefix)
        prefix = gBaseMCDir;

    configBuildPath(legacyNetConfigPath, sizeof(legacyNetConfigPath), prefix, "IPCONFIG.DAT");

    for (i = 0; i < CONFIG_INDEX_COUNT; i++) {
        configBuildPath(path, sizeof(path), prefix, configFilenames[i]);
        configMove(&configFiles[i], path);
    }

    configPrepareNotifications(prefix);
}

void configEnd()
{
    int index = 0;
    while (index < CONFIG_INDEX_COUNT) {
        config_set_t *configSet = &configFiles[index];

        configClear(configSet);
        free(configSet->filename);
        configSet->filename = NULL;
        index++;
    }
}

config_set_t *configAlloc(int type, config_set_t *configSet, char *fileName)
{
    int weAllocated = 0;
    if (!configSet) {
        configSet = (config_set_t *)malloc(sizeof(config_set_t));
        if (!configSet) // OOM: can't proceed without the struct
            return NULL;
        weAllocated = 1;
    }

    configSet->uid = ++currentUID;
    configSet->type = type;
    configSet->head = NULL;
    configSet->tail = NULL;
    // A set with no file on disk yet defaults to OUR format: it is the interoperable one (OPL,
    // OPL-Launcher, SAS and XMB all read it), so a NEW file should never be born libconfig. A file
    // that already exists overwrites this in configReadFileBuffer from what is actually on disk.
    configSet->format = CFG_FMT_LEGACY;
    if (fileName) {
        int length = strlen(fileName) + 1;
        configSet->filename = (char *)malloc(length * sizeof(char));
        if (!configSet->filename) { // OOM: clean up and bail
            if (weAllocated)
                free(configSet);
            return NULL;
        }
        memcpy(configSet->filename, fileName, length);
    } else
        configSet->filename = NULL;
    configSet->modified = 0;
    return configSet;
}

void configMove(config_set_t *configSet, const char *fileName)
{
    int length = strlen(fileName) + 1;
    // Use a temporary so a failed realloc doesn't clobber (and leak) the old pointer
    char *tmp = realloc(configSet->filename, length);
    if (!tmp)
        return;
    configSet->filename = tmp;
    memcpy(configSet->filename, fileName, length);
}

void configFree(config_set_t *configSet)
{
    configClear(configSet);
    free(configSet->filename);
    free(configSet);
}

// Deep-copy src into a fresh standalone (heap) config set. The clone is NOT registered in
// configFiles[] (configAlloc with a NULL set just mallocs one) and gets its own uid, so it is
// invisible to configGetByType and can be launched from + configFree()d without disturbing the
// live config. Faithfully copies every entry, including the runtime '#'-prefixed keys (Format/
// Startup/Size) the launch path needs. Returns NULL on OOM.
config_set_t *configClone(config_set_t *src)
{
    if (src == NULL)
        return NULL;

    config_set_t *dst = configAlloc(src->type, NULL, src->filename);
    if (dst == NULL)
        return NULL;

    struct config_value_t *it;
    for (it = src->head; it != NULL; it = it->next)
        addConfigValue(dst, it->key, it->val);

    dst->modified = 0;
    return dst;
}

config_set_t *configGetByType(int type)
{
    int index = 0;
    while (index < CONFIG_INDEX_COUNT) {
        config_set_t *configSet = &configFiles[index];

        if (configSet->type == type)
            return configSet;
        index++;
    }
    return NULL;
}

int configSetStr(config_set_t *configSet, const char *key, const char *value)
{
    if (!configKeyValidate(key))
        return 0;

    struct config_value_t *it = getConfigItemForName(configSet, key);

    if (it) {
        if (strncmp(it->val, value, sizeof(it->val)) != 0) {
            strncpy(it->val, value, sizeof(it->val));
            it->val[sizeof(it->val) - 1] = '\0';
            if (it->key[0] != '#')
                configSet->modified = 1;
        }
    } else {
        addConfigValue(configSet, key, value);
        if (key[0] != '#')
            configSet->modified = 1;
    }

    return 1;
}

// sets the value to point to the value str in the config. Do not overwrite - it will overwrite the string in config
int configGetStr(config_set_t *configSet, const char *key, const char **value)
{
    if (!configKeyValidate(key))
        return 0;

    struct config_value_t *it = getConfigItemForName(configSet, key);

    if (it) {
        *value = it->val;
        return 1;
    } else
        return 0;
}

int configGetStrCopy(config_set_t *configSet, const char *key, char *value, int length)
{
    const char *valref = NULL;
    if (configGetStr(configSet, key, &valref)) {
        strncpy(value, valref, length);
        value[length - 1] = '\0';
        return 1;
    } else {
        value[0] = '\0';
        return 0;
    }
}

int configSetInt(config_set_t *configSet, const char *key, const int value)
{
    char tmp[12];
    snprintf(tmp, sizeof(tmp), "%d", value);
    return configSetStr(configSet, key, tmp);
}

int configGetInt(config_set_t *configSet, const char *key, int *value)
{
    const char *valref = NULL;
    if (configGetStr(configSet, key, &valref)) {
        *value = atoi(valref);
        return 1;
    } else {
        return 0;
    }
}

int configSetColor(config_set_t *configSet, const char *key, unsigned char *color)
{
    char tmp[8];
    snprintf(tmp, sizeof(tmp), "#%02X%02X%02X", color[0], color[1], color[2]);
    return configSetStr(configSet, key, tmp);
}

int configGetColor(config_set_t *configSet, const char *key, unsigned char *color)
{
    const char *valref = NULL;
    if (configGetStr(configSet, key, &valref)) {
        strToColor(valref, color);
        return 1;
    } else {
        return 0;
    }
}

int configRemoveKey(config_set_t *configSet, const char *key)
{
    if (!configKeyValidate(key))
        return 0;

    struct config_value_t *val = configSet->head;
    struct config_value_t *prev = NULL;

    while (val) {
        if (strncmp(val->key, key, sizeof(val->key)) == 0) {
            if (key[0] != '#')
                configSet->modified = 1;

            if (val == configSet->tail)
                configSet->tail = prev;

            val = val->next;
            if (prev) {
                free(prev->next);
                prev->next = val;
            } else {
                free(configSet->head);
                configSet->head = val;
            }
        } else {
            prev = val;
            val = val->next;
        }
    }

    return 1;
}

void configMerge(config_set_t *dest, const config_set_t *source)
{
    struct config_value_t *val;

    for (val = source->head; val != NULL; val = val->next) {
        configSetStr(dest, val->key, val->val);
    }
}

static int configReadLegacyIP(void)
{
    config_set_t *configSet;
    char temp[16];

    int fd = openFile(legacyNetConfigPath, O_RDONLY);
    if (fd >= 0) {
        char ipconfig[256];
        int size = getFileSize(fd);
        // Bound the read to the stack buffer: the file size is untrusted and a
        // file larger than ipconfig[] would otherwise smash the stack.
        if (size < 0)
            size = 0;
        if (size >= (int)sizeof(ipconfig))
            size = sizeof(ipconfig) - 1;
        int rd = read(fd, ipconfig, size);
        if (rd < 0)
            rd = 0;
        ipconfig[rd] = '\0';
        close(fd);

        int n = sscanf(ipconfig, "%d.%d.%d.%d %d.%d.%d.%d %d.%d.%d.%d", &ps2_ip[0], &ps2_ip[1], &ps2_ip[2], &ps2_ip[3],
                       &ps2_netmask[0], &ps2_netmask[1], &ps2_netmask[2], &ps2_netmask[3],
                       &ps2_gateway[0], &ps2_gateway[1], &ps2_gateway[2], &ps2_gateway[3]);
        if (n != 12)
            return 0;

        configSet = &configFiles[CONFIG_INDEX_NETWORK];

        snprintf(temp, sizeof(temp), "%d.%d.%d.%d", ps2_ip[0], ps2_ip[1], ps2_ip[2], ps2_ip[3]);
        configSetStr(configSet, CONFIG_NET_PS2_IP, temp);
        snprintf(temp, sizeof(temp), "%d.%d.%d.%d", ps2_netmask[0], ps2_netmask[1], ps2_netmask[2], ps2_netmask[3]);
        configSetStr(configSet, CONFIG_NET_PS2_NETM, temp);
        snprintf(temp, sizeof(temp), "%d.%d.%d.%d", ps2_gateway[0], ps2_gateway[1], ps2_gateway[2], ps2_gateway[3]);
        configSetStr(configSet, CONFIG_NET_PS2_GATEW, temp);
        // The legacy format has no setting for the DNS server, so duplicate the gateway address.
        configSetStr(configSet, CONFIG_NET_PS2_DNS, temp);

        return 1;
    }

    return 0;
}

// dst has to have 5 bytes space
void configGetDiscIDBinary(config_set_t *configSet, void *dst)
{
    memset(dst, 0, 5);

    const char *gid = NULL;
    if (configGetStr(configSet, CONFIG_ITEM_DNAS, &gid)) {
        // convert from hex to binary
        char *cdst = dst;
        int p = 0;
        while (*gid && p < 10) {
            int dv = -1;

            while (dv < 0 && *gid) // skip spaces, etc
                dv = fromHex(*(gid++));

            if (dv < 0)
                break;

            *cdst = *cdst * 16 + dv;
            if ((++p & 1) == 0)
                cdst++;
        }
    }
}

// Defined further down with the key table; the reader below needs the wOPL->ours direction.
static const char *cfgWoplToOurs(const char *key);

// Parse ONE line of a libconfig file into our flat key space.
//
// GROUPS FLATTEN ONTO OUR EXISTING COMPOSED KEYS. Our legacy parser turns
//     main0:            ->  prefix "main0"
//         type=Background   ->  key "main0_type"   (parsePrefix + the "%s_%s" compose below)
// and libconfig writes the same data as
//     main0 : { type = "Background"; };
// so joining group+key with '_' lands on the IDENTICAL key. Themes therefore need no mapping at all.
// One level of nesting is all wOPL emits (build_per_game / their theme builder).
//
// EMPTY STRING IS *ABSENT*, NOT "". wOPL's set_str has no skip-if-empty and init_per_game_cfg memsets
// to zero, so EVERY wOPL per-game file literally contains `dnas = ""; alt_startup = ""; vmc1 = "";
// vmc2 = "";`. This fork's invariant is absent==unset -- we never write an empty $AltStartup or $VMC.
// Storing "" would tell OPL "an alt-startup IS configured, and it is the empty string", which breaks
// the launch. So an empty string is DROPPED on read. configWrite re-emits the empties for the keys
// wOPL expects, so their file stays valid for them (see cfgWriteLibconfigLine).
static void cfgReadLibconfigLine(char *line, char *group, size_t groupSize, config_set_t *configSet)
{
    char *p = line, *eq, *end, *v;
    char key[CONFIG_KEY_NAME_LEN], composed[2 * CONFIG_KEY_NAME_LEN];
    size_t klen, vlen;

    while (isWS(*p))
        ++p;

    if (*p == '\0' || *p == '#' || (p[0] == '/' && (p[1] == '/' || p[1] == '*')))
        return;

    // "};" or "}" closes the current group; "{" alone is the brace of a group header we already took.
    if (*p == '}') {
        group[0] = '\0';
        return;
    }
    if (*p == '{')
        return;

    end = p + strlen(p);
    while (end > p && isWS(end[-1]))
        --end;
    if (end > p && end[-1] == ';')
        --end; // drop the statement terminator
    while (end > p && isWS(end[-1]))
        --end;

    eq = strchr(p, '=');

    // Group header: "name :" or "name : {" -- no '=' before the ':'.
    if (eq == NULL || (strchr(p, ':') != NULL && strchr(p, ':') < eq)) {
        char *colon = strchr(p, ':');
        if (colon != NULL) {
            char *kend = colon;
            while (kend > p && isWS(kend[-1]))
                --kend;
            klen = (size_t)(kend - p);
            if (klen > 0 && klen < groupSize) {
                memcpy(group, p, klen);
                group[klen] = '\0';
            }
        }
        return;
    }

    { // scalar: key = value
        char *kend = eq;
        while (kend > p && isWS(kend[-1]))
            --kend;
        klen = (size_t)(kend - p);
        if (klen == 0 || klen >= sizeof(key))
            return; // nothing usable, or a key longer than we can store -- never truncate into a hit
        memcpy(key, p, klen);
        key[klen] = '\0';
    }

    v = eq + 1;
    while (v < end && isWS(*v))
        ++v;

    if (v < end && *v == '"') { // quoted string: strip the quotes
        ++v;
        if (end > v && end[-1] == '"')
            --end;
    }

    vlen = (end > v) ? (size_t)(end - v) : 0;
    if (vlen == 0)
        return; // EMPTY == ABSENT (see the note above) -- do NOT store ""
    if (vlen >= CONFIG_KEY_VALUE_LEN)
        vlen = CONFIG_KEY_VALUE_LEN - 1;

    { // libconfig bools -> the 0/1 our getters expect
        char val[CONFIG_KEY_VALUE_LEN];
        memcpy(val, v, vlen);
        val[vlen] = '\0';

        if (strcmp(val, "true") == 0)
            strcpy(val, "1");
        else if (strcmp(val, "false") == 0)
            strcpy(val, "0");

        if (group[0])
            snprintf(composed, sizeof(composed), "%s_%s", group, key);
        else
            snprintf(composed, sizeof(composed), "%s", key);

        { // translate wOPL's per-game key names to ours; anything else passes through untouched
            const char *ours = cfgWoplToOurs(composed);
            configSetStr(configSet, ours != NULL ? ours : composed, val);
        }
    }
}

// ---- wOPL per-game key translation ----------------------------------------------------------------
// wOPL's per-game migration does NOT merely reformat: it RENAMES every key and nests most of them.
// Their build_per_game writes `compat`, `dma`, and groups `gsm`/`cheat`/`pademu`/`padmacro`/`osd`,
// where we use `$Compatibility`, `$DMA`, `$EnableGSM` and friends. Parsing their syntax is therefore
// only half the job -- without this table we would read the file perfectly and hand OPL a set of keys
// nothing looks up, and every per-game setting would silently revert to defaults.
//
// The left column is the key AFTER group flattening (cfgReadLibconfigLine joins "gsm : { enable = 1; }"
// into "gsm_enable"), so the group structure round-trips through cfgMatchGroup on write.
//
// ONLY per-game keys live here. A key that is not in this table passes through UNCHANGED, which is what
// makes themes (`main0_type`) and app title.cfg (`title`/`boot`/`argv1`) work without any mapping.
//
// SEMANTICS VERIFIED, NOT ASSUMED:
//   core_loader -- IDENTICAL on both sides: 0 = the host loader, 1 = Neutrino (theirs:
//                  CORE_LOADER_WOPL/CORE_LOADER_NEUTRINO in config_wopl.h:12-13; ours: the stored
//                  $CoreLoader int, guigame.c:1059 "0=<OPL>, 1=Neutrino"). No transform needed.
//   vmc1/vmc2   -- THEIRS IS 1-BASED, OURS IS 0-BASED ($VMC_0/$VMC_1, composed at config.c "%s_%d").
//                  Getting this backwards would load slot 2's card into slot 1.
static const struct
{
    const char *wopl;
    const char *ours;
} cfgWoplPerGameKeys[] = {
    {"compat", CONFIG_ITEM_COMPAT},
    {"dma", CONFIG_ITEM_DMA},
    {"core_loader", CONFIG_ITEM_CORE_LOADER},
    {"dnas", CONFIG_ITEM_DNAS},
    {"alt_startup", CONFIG_ITEM_ALTSTARTUP},
    {"vmc1", CONFIG_ITEM_VMC "_0"}, // 1-based -> 0-based
    {"vmc2", CONFIG_ITEM_VMC "_1"},
    {"gsm_source", CONFIG_ITEM_GSMSOURCE},
    {"gsm_enable", CONFIG_ITEM_ENABLEGSM},
    {"gsm_vmode", CONFIG_ITEM_GSMVMODE},
    {"gsm_x_offset", CONFIG_ITEM_GSMXOFFSET},
    {"gsm_y_offset", CONFIG_ITEM_GSMYOFFSET},
    {"gsm_field_fix", CONFIG_ITEM_GSMFIELDFIX},
    {"cheat_source", CONFIG_ITEM_CHEATSSOURCE},
    {"cheat_enable", CONFIG_ITEM_ENABLECHEAT},
    {"cheat_mode", CONFIG_ITEM_CHEATMODE},
    {"cheat_enable_image", CONFIG_ITEM_ENABLEIMAGE},
    {"pademu_source", CONFIG_ITEM_PADEMUSOURCE},
    {"pademu_enable", CONFIG_ITEM_ENABLEPADEMU},
    {"pademu_settings", CONFIG_ITEM_PADEMUSETTINGS},
    {"padmacro_source", CONFIG_ITEM_PADMACROSOURCE},
    {"padmacro_settings", CONFIG_ITEM_PADMACROSETTINGS},
    {"osd_source", CONFIG_ITEM_OSD_SETTINGS_SOURCE},
    {"osd_enable", CONFIG_ITEM_OSD_SETTINGS_ENABLE},
    {"osd_lang_id", CONFIG_ITEM_OSD_SETTINGS_LANGID},
    {"osd_tv_aspect", CONFIG_ITEM_OSD_SETTINGS_TV_ASP},
    {"osd_vmode", CONFIG_ITEM_OSD_SETTINGS_VMODE},
    {NULL, NULL},
};

// Their key -> ours. Returns NULL when the key is not one of theirs (pass it through unchanged).
static const char *cfgWoplToOurs(const char *key)
{
    int i;
    for (i = 0; cfgWoplPerGameKeys[i].wopl != NULL; ++i) {
        if (strcmp(key, cfgWoplPerGameKeys[i].wopl) == 0)
            return cfgWoplPerGameKeys[i].ours;
    }
    return NULL;
}

// Ours -> theirs. Returns NULL when we have no wOPL equivalent -- those keys ($NeutrinoArgs,
// $NeutrinoVideo, $VMCDisable_n, $ConfigSource ...) are emitted at ROOT verbatim instead of being
// dropped. wOPL's parse_per_game is pure config_lookup by name and never enumerates root children, so
// it ignores them silently while they survive for us: format inheritance with no data loss either way.
static const char *cfgOursToWopl(const char *key)
{
    int i;
    for (i = 0; cfgWoplPerGameKeys[i].wopl != NULL; ++i) {
        if (strcmp(key, cfgWoplPerGameKeys[i].ours) == 0)
            return cfgWoplPerGameKeys[i].wopl;
    }
    return NULL;
}

// True when the value is a bare libconfig scalar (int or bool) and therefore must NOT be quoted.
// Everything else is emitted as a quoted string, which is what libconfig's own reader expects and
// what wOPL's cfgGetStr asks for. A leading '-' is allowed (negative ints).
static int cfgValueIsBareScalar(const char *v)
{
    const char *p = v;

    if (strcmp(v, "true") == 0 || strcmp(v, "false") == 0)
        return 1;
    if (*p == '-')
        ++p;
    if (*p == '\0')
        return 0;
    for (; *p; ++p) {
        if (!isdigit((unsigned char)*p))
            return 0;
    }
    return 1;
}

// Emit one "key = value;" line, quoting and escaping as libconfig requires.
static void cfgWriteLibconfigLine(file_buffer_t *fileBuffer, const char *indent, const char *key, const char *val)
{
    // Sized for the WORST case, because snprintf TRUNCATES rather than overflows and a truncated line
    // loses its closing quote+semicolon -- which corrupts the file for wOPL and for us alike (Gemini
    // review of #184). Worst case: indent(2) + key(CONFIG_KEY_NAME_LEN-1 = 31) + ` = "` (4) +
    // every one of the value's 255 chars escaped (510) + `";\n` (3) + NUL = 551. A plain 512 was short.
    char line[CONFIG_KEY_NAME_LEN + CONFIG_KEY_VALUE_LEN * 2 + 16];

    if (cfgValueIsBareScalar(val)) {
        snprintf(line, sizeof(line), "%s%s = %s;\n", indent, key, val);
        writeFileBuffer(fileBuffer, line, strlen(line));
        return;
    }

    { // quoted string: escape '\' and '"' so a path or a title with a quote cannot break the file
        char esc[CONFIG_KEY_VALUE_LEN * 2];
        size_t i = 0;
        const char *p;

        for (p = val; *p && i + 2 < sizeof(esc); ++p) {
            if (*p == '\\' || *p == '"')
                esc[i++] = '\\';
            esc[i++] = *p;
        }
        esc[i] = '\0';

        snprintf(line, sizeof(line), "%s%s = \"%s\";\n", indent, key, esc);
        writeFileBuffer(fileBuffer, line, strlen(line));
    }
}

// Write the whole set back as libconfig, re-nesting the groups we flattened on read.
//
// Our store is flat, and cfgReadLibconfigLine joined "gsm : { enable = 1; }" into "gsm_enable". To
// round-trip we must split on the LAST '_' whose prefix matches a group we actually saw, which we
// cannot know from the key alone ("alt_startup" is a root key, not group "alt" key "startup"). So we
// only re-nest keys whose group prefix appeared in wOPL's OWN schema -- anything else stays at root.
// A key we do not recognise is emitted at root verbatim rather than dropped: wOPL's parse_per_game is
// pure config_lookup by name and never enumerates children, so unknown settings are silently ignored
// by them and survive for us. Nobody loses data in either direction.
static const char *const cfgLibconfigGroups[] = {"gsm", "cheat", "pademu", "padmacro", "osd", NULL};

static const char *cfgMatchGroup(const char *key, int *keyOffset)
{
    int i;

    for (i = 0; cfgLibconfigGroups[i] != NULL; ++i) {
        size_t glen = strlen(cfgLibconfigGroups[i]);
        if (strncmp(key, cfgLibconfigGroups[i], glen) == 0 && key[glen] == '_' && key[glen + 1] != '\0') {
            *keyOffset = (int)glen + 1;
            return cfgLibconfigGroups[i];
        }
    }
    return NULL;
}

static void cfgWriteLibconfig(file_buffer_t *fileBuffer, config_set_t *configSet)
{
    struct config_value_t *cur;
    int i;

    // Pass 1: every root-level scalar -- i.e. anything that does not translate into one of wOPL's
    // groups. A key with no wOPL equivalent lands here verbatim (see cfgOursToWopl) rather than being
    // dropped, so our Neutrino/VMCDisable settings survive a wOPL-format file untouched.
    for (cur = configSet->head; cur; cur = cur->next) {
        const char *w;
        int off;

        if (cur->key[0] == '\0' || cur->key[0] == '#')
            continue;

        w = cfgOursToWopl(cur->key);
        if (w != NULL && cfgMatchGroup(w, &off) != NULL)
            continue; // belongs to a group; pass 2 emits it

        cfgWriteLibconfigLine(fileBuffer, "", w != NULL ? w : cur->key, cur->val);
    }

    // Pass 2: one block per group, in schema order, skipping groups with nothing in them.
    for (i = 0; cfgLibconfigGroups[i] != NULL; ++i) {
        char line[128];
        int any = 0;

        for (cur = configSet->head; cur; cur = cur->next) {
            const char *w, *g;
            int off;

            if (cur->key[0] == '\0' || cur->key[0] == '#')
                continue;

            w = cfgOursToWopl(cur->key);
            if (w == NULL)
                continue;
            g = cfgMatchGroup(w, &off);
            if (g == NULL || strcmp(g, cfgLibconfigGroups[i]) != 0)
                continue;

            if (!any) {
                snprintf(line, sizeof(line), "%s :\n{\n", cfgLibconfigGroups[i]);
                writeFileBuffer(fileBuffer, line, strlen(line));
                any = 1;
            }
            cfgWriteLibconfigLine(fileBuffer, "  ", w + off, cur->val);
        }

        if (any) {
            snprintf(line, sizeof(line), "};\n");
            writeFileBuffer(fileBuffer, line, strlen(line));
        }
    }
}

static int configReadFileBuffer(file_buffer_t *fileBuffer, config_set_t *configSet)
{
    char *line;
    unsigned int lineno = 0;
    int fmt = CFG_FMT_LEGACY, fmtLatched = 0;
    char group[CONFIG_KEY_NAME_LEN];

    char prefix[CONFIG_KEY_NAME_LEN];
    memset(prefix, 0, sizeof(prefix));
    memset(group, 0, sizeof(group));

    while (readFileBuffer(fileBuffer, &line)) {
        lineno++;

        // Latch the format from the first line that carries evidence. Blank/comment lines say nothing,
        // so keep looking. An empty or comments-only file never latches and keeps the CFG_FMT_LEGACY
        // default from configAlloc -- correct, since a file with no content to preserve should be
        // written back in the interoperable format.
        if (!fmtLatched) {
            int c = cfgClassifyLine(line);
            if (c == CFG_LINE_BLANK || c == CFG_LINE_COMMENT)
                continue;
            fmt = (c == CFG_LINE_LIBCONFIG) ? CFG_FMT_LIBCONFIG : CFG_FMT_LEGACY;
            fmtLatched = 1;
            configSet->format = fmt;
        }

        if (fmt == CFG_FMT_LIBCONFIG) {
            cfgReadLibconfigLine(line, group, sizeof(group), configSet);
            continue;
        }

        char key[CONFIG_KEY_NAME_LEN], val[CONFIG_KEY_VALUE_LEN];
        memset(key, 0, sizeof(key));
        memset(val, 0, sizeof(val));

        if (splitAssignment(line, key, sizeof(key), val, sizeof(val))) {
            /* if the line does not start with whitespace,
             * the prefix ends and we have to reset it
             */
            if (!isWS(line[0]))
                memset(prefix, 0, sizeof(prefix));

            // insert config value
            if (prefix[0]) {
                // we have a prefix
                char composedKey[2 * CONFIG_KEY_NAME_LEN];

                snprintf(composedKey, sizeof(composedKey), "%s_%s", prefix, key);
                configSetStr(configSet, composedKey, val);
            } else {
                configSetStr(configSet, key, val);
            }
        } else if (parsePrefix(line, prefix, sizeof(prefix))) {
            // prefix is set, that's about it
        } else {
            LOG("CONFIG Malformed file '%s' line %d: '%s'\n", configSet->filename, lineno, line);
        }
    }
    configSet->modified = 0;
    return 1;
}

int configReadBuffer(config_set_t *configSet, const void *buffer, int size)
{
    int ret;
    file_buffer_t *fileBuffer = openFileBufferBuffer(0, buffer, size);
    if (!fileBuffer) {
        configSet->modified = 0;
        return 0;
    }

    ret = configReadFileBuffer(fileBuffer, configSet);

    closeFileBuffer(fileBuffer);
    return ret;
}

// RiptOPL master config was renamed conf_riptopl.cfg -> settings_riptopl.cfg. Given the built
// "<dir>/settings_riptopl.cfg" path, produce its legacy "<dir>/conf_riptopl.cfg" sibling so an
// existing install's settings still load (read-fallback); the next save writes the new name.
static void configBuildLegacyOplPath(const char *path, char *out, int outSize)
{
    const char *slash = strrchr(path, '/');
    if (slash != NULL)
        snprintf(out, outSize, "%.*s%s", (int)(slash - path) + 1, path, CONFIG_OPL_FILENAME_LEGACY);
    else
        snprintf(out, outSize, "%s", CONFIG_OPL_FILENAME_LEGACY);
}

// wOPL does not only reformat -- for three files it RELOCATES the data and leaves nothing at the name
// we look for. Format detection cannot help with a file that is not there, so map our name onto the
// wOPL one and read that instead. We honour the setup that exists rather than forcing ours back on top:
// no rewrite, no un-migration, no "restore" -- we just read where the data actually lives now.
//
//   conf_theme.cfg   -> wopl_theme.cfg        the original is UNLINKED with NO backup (their .bak
//                                             gates on the DESTINATION already existing, which for a
//                                             renamed file it never does), so this fallback is the
//                                             ONLY way a wOPL-touched theme still renders for us.
//   conf_network.cfg -> wopl_network.cfg      original renamed to .bak
//   conf_game.cfg    -> wopl_global_game.cfg  original renamed to .bak
//
// Returns 0 when this filename has no wOPL counterpart.
static int configBuildWoplPath(const char *path, char *out, int outSize)
{
    static const struct
    {
        const char *ours;
        const char *theirs;
    } map[] = {
        {"conf_theme.cfg", "wopl_theme.cfg"},
        {"conf_network.cfg", "wopl_network.cfg"},
        {"conf_game.cfg", "wopl_global_game.cfg"},
        {NULL, NULL},
    };
    const char *slash;
    int dirLen, i;

    if (path == NULL)
        return 0;

    slash = strrchr(path, '/');
    dirLen = (slash != NULL) ? (int)(slash - path) + 1 : 0;

    for (i = 0; map[i].ours != NULL; ++i) {
        const char *base = path + dirLen;
        if (strcmp(base, map[i].ours) != 0)
            continue;
        if (dirLen + (int)strlen(map[i].theirs) + 1 > outSize)
            return 0; // would not fit: leave it alone rather than compose a wrong path
        memcpy(out, path, dirLen);
        strcpy(out + dirLen, map[i].theirs);
        return 1;
    }
    return 0;
}

static int configPathIsRawApa(const char *path)
{
    // hddN: is not a filesystem path. It is PS2SDK's raw APA partition namespace, where
    // O_CREAT means "create an APA partition". Config files must always use mounted PFS
    // (pfsN:) instead. Keep this guard here even if higher-level path resolution regresses.
    return path != NULL && !strncmp(path, "hdd", 3) && path[3] >= '0' && path[3] <= '9' && path[4] == ':';
}

int configRead(config_set_t *configSet)
{
    if (configSet != NULL && configPathIsRawApa(configSet->filename)) {
        LOG("CONFIG refusing raw APA read path %s; use a mounted pfsN: path\n", configSet->filename);
        return 0;
    }
    int ret;
    file_buffer_t *fileBuffer = openFileBuffer(configSet->filename, O_RDONLY, 0, 4096);

    if (fileBuffer == NULL && configSet->filename != NULL) {
        // Our name is absent -- wOPL may have moved this file's data to its own name (see above).
        char woplPath[256];
        if (configBuildWoplPath(configSet->filename, woplPath, sizeof(woplPath))) {
            fileBuffer = openFileBuffer(woplPath, O_RDONLY, 0, 4096);
            if (fileBuffer != NULL) {
                LOG("CONFIG %s absent; reading wOPL's %s instead\n", configSet->filename, woplPath);
                // Point the set at the file we actually read, so a later save updates THAT file rather
                // than resurrecting our name beside it and leaving the user with two configs that
                // disagree. Their layout, honoured end to end.
                configMove(configSet, woplPath);
            }
        }
    }

    if (fileBuffer == NULL && configSet->type == CONFIG_OPL && configSet->filename != NULL) {
        // Migration: existing installs have the legacy conf_riptopl.cfg, not settings_riptopl.cfg.
        // Read the legacy file from the same dir so settings aren't lost; the next save writes the
        // new name. configSet->filename stays the new name, so configWrite migrates transparently.
        char legacyPath[256];
        configBuildLegacyOplPath(configSet->filename, legacyPath, sizeof(legacyPath));
        fileBuffer = openFileBuffer(legacyPath, O_RDONLY, 0, 4096);
        if (fileBuffer != NULL)
            LOG("CONFIG migrating settings from legacy %s\n", legacyPath);
    }
    if (!fileBuffer) {
        LOG("CONFIG No file %s.\n", configSet->filename);
        configSet->modified = 0;
        return 0;
    }

    ret = configReadFileBuffer(fileBuffer, configSet);

    closeFileBuffer(fileBuffer);
    return ret;
}

// Cap on the pre-write snapshot kept in RAM to roll back a failed save. Real config files are a few
// KB; this bounds a corrupt/huge on-disk size so a bad stat can never malloc something absurd.
#define CONFIG_MAX_RESTORE_BYTES (256 * 1024)

int configWrite(config_set_t *configSet)
{
    // A config can be re-homed after it was read (legacy migration, missing-config recovery,
    // boot-home self-migration). configMove/configSetMove intentionally preserve the in-memory
    // values and their modified flag, so an unchanged set may now point at a destination file that
    // does not exist. Returning the normal "unmodified = success" result in that state is a false
    // positive: the UI reports Saved, but nothing was materialized at the new home. If a populated
    // set is clean yet its CURRENT filename is absent, make this explicit save create it. Existing
    // files stay on the zero-write fast path; empty sets are not manufactured merely because their
    // optional file is absent. This is filesystem-only and never bypasses the raw-APA firewall below.
    if (!configSet->modified && configSet->filename != NULL && configSet->head != NULL) {
        iox_stat_t stat;
        if (fileXioGetStat(configSet->filename, &stat) < 0) {
            LOG("CONFIG current save destination %s is absent; materializing populated config\n", configSet->filename);
            configSet->modified = 1;
        }
    }

    if (configSet->modified) {
        if (configSet->filename == NULL)
            return 0; // in-memory-only config set: nothing to persist (and openFile would deref NULL)

        // Absolute safety boundary: never let a config save reach PS2SDK's raw APA namespace.
        // A past custom path such as hdd0:/+OPL/settings_riptopl.cfg reached open(..., O_CREAT),
        // which the APA driver interprets as partition creation rather than file creation.
        if (configPathIsRawApa(configSet->filename)) {
            LOG("CONFIG blocked dangerous raw APA write path %s; use mounted pfsN: instead\n", configSet->filename);
            gLastSaveErrno = EACCES;
            return 0;
        }

        // The write is NON-ATOMIC: openFileBuffer opens with O_TRUNC, emptying the existing good file
        // BEFORE the new content is flushed. On flaky media (a wedged HDD -- the reported case) the
        // flush -- or even openFileBuffer's own buffer malloc, AFTER openFile's O_TRUNC already
        // emptied the file -- can fail, leaving a 0-byte config that reads back as all-defaults
        // ("configuration appeared WIPED"); the old code cleared modified + returned success
        // regardless. PS2 filesystems lack reliable atomic rename (see system.c sysSyncNeutrinoIp), so
        // snapshot the current on-disk bytes first and restore them if the save fails for ANY reason.
        // PS2 MEMORY CARD: write exactly the way upstream OPL always has -- ONE open, write, close --
        // and skip the snapshot/verify/restore machinery below.
        //
        // Hardware (#340 session): the first save to a card SUCCEEDS and every save after it fails
        // with "could not write settings to mc0: (error 24)"; the VCD BDMA marker write to the card
        // fails the same way. 24 is EMFILE. What separates the first save from the rest is exactly
        // this block: the snapshot READ only opens anything when the file ALREADY EXISTS. Upstream
        // performs a single open here and has done so reliably for years, so the extra opens are the
        // new variable, and the card's driver keeps a far smaller descriptor table than a block
        // device. The wipe this machinery defends against (#184 -- O_TRUNC empties a good file, then
        // a flaky device fails the flush) was reported on a wedged HDD/MMCE, not on a memory card,
        // and a failed card write still reports honestly and keeps modified=1 for a retry.
        // "mmce" does not match: its second character is 'm', not 'c'.
        const int singleOpenDevice = (strncmp(configSet->filename, "mc", 2) == 0);

        char *original = NULL;
        int originalLen = 0;
        int rfd = singleOpenDevice ? -1 : openFile(configSet->filename, O_RDONLY);
        if (rfd >= 0) {
            int sz = getFileSize(rfd);
            if (sz > 0 && sz <= CONFIG_MAX_RESTORE_BYTES) {
                original = (char *)malloc(sz);
                if (original != NULL) {
                    originalLen = read(rfd, original, sz);
                    if (originalLen != sz) { // partial read -> unusable snapshot, drop it
                        free(original);
                        original = NULL;
                        originalLen = 0;
                    }
                }
            }
            close(rfd);
        }

        int ok = 0;
        // Clear errno before EACH stage that can set gLastSaveErrno. Without this the reported code
        // can be a leftover from an unrelated earlier call -- a failure path that never sets errno
        // then "reports" whatever was lying around, which is how a save failure can name a
        // misleading cause (the stale-errno trap flagged in the #340 handoff).
        errno = 0;
        file_buffer_t *fileBuffer = openFileBuffer(configSet->filename, O_WRONLY | O_CREAT | O_TRUNC, 0, 4096);
        if (fileBuffer) {
            char line[512];

            bgmMute();

            // FORMAT INHERITANCE: emit whatever syntax this file arrived in. A file that was libconfig
            // when we read it is written back as libconfig; a legacy file stays legacy. We never
            // convert a user's file in either direction -- that is the whole point. Before this, a
            // libconfig file hit the "%s=%s\r\n" loop below and was DESTROYED on the first save.
            if (configSet->format == CFG_FMT_LIBCONFIG) {
                cfgWriteLibconfig(fileBuffer, configSet);
            } else {
                struct config_value_t *cur = configSet->head;
                while (cur) {
                    if ((cur->key[0] != '\0') && (cur->key[0] != '#')) {
                        snprintf(line, sizeof(line), "%s=%s\r\n", cur->key, cur->val); // add windows CR+LF (0x0D 0x0A)
                        writeFileBuffer(fileBuffer, line, strlen(line));
                    }

                    // and advance
                    cur = cur->next;
                }
            }

            // Capture the exact bytes about to be flushed, for the read-back verify below. The capture
            // is COMPLETE only when nothing was flushed early (totalQueued == available) -- true for
            // every real config (< 4 KB buffer). On a partial capture the rescue is skipped entirely;
            // it never guesses.
            char *intended = NULL;
            unsigned int intendedLen = 0;
            if (fileBuffer->available > 0 && fileBuffer->totalQueued == fileBuffer->available) {
                intended = (char *)malloc(fileBuffer->available);
                if (intended != NULL) {
                    memcpy(intended, fileBuffer->buffer, fileBuffer->available);
                    intendedLen = fileBuffer->available;
                }
            }

            errno = 0; // fresh, so a close failure reports its OWN code (see the note above)
            ok = (closeFileBuffer(fileBuffer) == 0);
            if (!ok) {
                gLastSaveErrno = errno;
                LOG("CONFIG write to %s: FLUSH/CLOSE stage failed, errno %d\n", configSet->filename, gLastSaveErrno);
            }

            // EVIDENCE OVER STATUS CODES (#245, temporal bisect): on some MMCE clone firmware the
            // write can LAND while the close reply is junk/timed out -- and honoring that status
            // verbatim did two bad things: reported a persisted save as failed, and (worse) let the
            // restore below O_TRUNC-rewrite the ORIGINAL bytes over the new ones when the new config
            // was shorter, actively UN-saving the user's change. So on a failed close, read the file
            // back and byte-compare against the captured intent: identical bytes = the save
            // demonstrably PERSISTED, whatever the status said -> success (no restore, no toast). Any
            // mismatch, short read, or unreadable file leaves the failure standing and the restore
            // path intact -- this can only ever convert failure->success on PROOF, so the #184
            // silent-wipe guard is not weakened.
            // ...but NEVER on pfs: PFS commits data+metadata through a shared IOP-side block cache, so
            // after a FAILED close a fresh open+read can be satisfied entirely from that still-dirty
            // cache -- proving nothing about the platter. There, honest failure + retry is the safe
            // behavior. MMCE is cacheless (the read round-trips to the card), which is what makes the
            // rescue's evidence real on the device this targets.
            if (!ok && intended != NULL && !singleOpenDevice && strncmp(configSet->filename, "pfs", 3) != 0) {
                int vfd = openFile(configSet->filename, O_RDONLY);
                if (vfd >= 0) {
                    int vsz = getFileSize(vfd);
                    if (vsz == (int)intendedLen) {
                        char *readBack = (char *)malloc(intendedLen);
                        if (readBack != NULL) {
                            if (read(vfd, readBack, intendedLen) == (int)intendedLen &&
                                memcmp(readBack, intended, intendedLen) == 0) {
                                LOG("CONFIG write to %s: close reported failure but read-back verifies all %u bytes -- treating as SAVED\n",
                                    configSet->filename, intendedLen);
                                ok = 1;
                                gLastSaveErrno = 0;
                            }
                            free(readBack);
                        }
                    }
                    close(vfd);
                }
            }
            free(intended);
            bgmUnMute();
        } else {
            gLastSaveErrno = errno;
            // captured HERE before the restore-path opens below clobber it
            LOG("CONFIG write to %s: OPEN stage failed, errno %d\n", configSet->filename, gLastSaveErrno);
        }

        if (!ok) {
            // The save failed -- either openFileBuffer failed AFTER openFile's O_TRUNC emptied the
            // file (OOM on the buffer alloc), or the flush itself failed post-truncate. Restore the
            // original bytes ONLY if the file is actually damaged now (shorter than the snapshot): a
            // plain open-failure leaves the file intact, and O_TRUNC+rewriting an intact file could
            // itself corrupt it on a second failure. Keep modified=1 so a later save retries, and
            // return 0 so saveConfig shows "Error saving settings" instead of a false "Saved".
            if (original != NULL) {
                int cfd = openFile(configSet->filename, O_RDONLY);
                int curLen = (cfd >= 0) ? getFileSize(cfd) : 0; // unreadable/gone -> treat as damaged
                if (cfd >= 0)
                    close(cfd);
                if (curLen < originalLen) {
                    int wfd = openFile(configSet->filename, O_WRONLY | O_CREAT | O_TRUNC);
                    if (wfd >= 0) {
                        write(wfd, original, originalLen);
                        close(wfd);
                    }
                    LOG("CONFIG write to %s FAILED -- restored %d original bytes\n", configSet->filename, originalLen);
                }
            } else {
                LOG("CONFIG write to %s FAILED (no snapshot to restore)\n", configSet->filename);
            }
            free(original);
            return 0;
        }

        free(original);
        configSet->modified = 0;
        return 1;
    }
    return 1;
}

int configGetStat(config_set_t *configSet, iox_stat_t *stat)
{
    return (fileXioGetStat(configSet->filename, stat) >= 0 ? 1 : 0);
}

void configClear(config_set_t *configSet)
{
    while (configSet->head) {
        struct config_value_t *cur = configSet->head;
        configSet->head = cur->next;

        free(cur);
    }

    configSet->head = NULL;
    configSet->tail = NULL;
    configSet->modified = 1;
}

int configReadMulti(int types)
{
    int result = 0, index;

    for (index = 0; index < CONFIG_INDEX_COUNT; index++) {
        config_set_t *configSet = &configFiles[index];

        if (configSet->type & types) {
            configClear(configSet);
            if (configRead(configSet)) {
                result |= configSet->type;
                // Record the file source BEFORE higher-level recovery re-homes the live config sets
                // for a future save. Load notifications must describe evidence, not ownership policy.
                if (configSet->type == CONFIG_OPL)
                    configPrepareLoadNotification(configSet->filename);
            }
        }
    }

    // If the network configuration is to be loaded and one cannot be loaded, attempt to load from the legacy network config file.
    if ((types & CONFIG_NETWORK) && !(result & CONFIG_NETWORK))
        if (configReadLegacyIP())
            result |= CONFIG_NETWORK;

    return result;
}

int configWriteMulti(int types)
{
    int result = 0, index;

    for (index = 0; index < CONFIG_INDEX_COUNT; index++) {
        config_set_t *configSet = &configFiles[index];

        if (configSet->type & types)
            result += configWrite(configSet);
    }

    return result;
}

void configGetVMC(config_set_t *configSet, char *vmc, int length, int slot)
{
    char gkey[CONFIG_KEY_NAME_LEN];
    snprintf(gkey, sizeof(gkey), "%s_%d", CONFIG_ITEM_VMC, slot);
    configGetStrCopy(configSet, gkey, vmc, length);
}

void configSetVMC(config_set_t *configSet, const char *vmc, int slot)
{
    char gkey[CONFIG_KEY_NAME_LEN];
    if (vmc[0] == '\0') {
        configRemoveVMC(configSet, slot);
        return;
    }
    snprintf(gkey, sizeof(gkey), "%s_%d", CONFIG_ITEM_VMC, slot);
    configSetStr(configSet, gkey, vmc);
}

void configRemoveVMC(config_set_t *configSet, int slot)
{
    char gkey[CONFIG_KEY_NAME_LEN];
    snprintf(gkey, sizeof(gkey), "%s_%d", CONFIG_ITEM_VMC, slot);
    configRemoveKey(configSet, gkey);
}

// Per-slot VMC disable (parity-audit #14): the toggle preserves the configured card name
// ($VMC_N stays) but makes the Neutrino launch skip that slot's -mc arg -- disable-without-delete,
// the same convention as the $-prefix on free-text args. Absent key == 0 == enabled.
void configGetVMCDisable(config_set_t *configSet, int slot, int *disabled)
{
    char gkey[CONFIG_KEY_NAME_LEN];
    snprintf(gkey, sizeof(gkey), "%s_%d", CONFIG_ITEM_VMC_DISABLE, slot);
    configGetInt(configSet, gkey, disabled);
}

void configSetVMCDisable(config_set_t *configSet, int slot, int disabled)
{
    char gkey[CONFIG_KEY_NAME_LEN];
    if (!disabled) { // default: keep configs clean, store nothing
        configRemoveVMCDisable(configSet, slot);
        return;
    }
    snprintf(gkey, sizeof(gkey), "%s_%d", CONFIG_ITEM_VMC_DISABLE, slot);
    configSetInt(configSet, gkey, disabled);
}

void configRemoveVMCDisable(config_set_t *configSet, int slot)
{
    char gkey[CONFIG_KEY_NAME_LEN];
    snprintf(gkey, sizeof(gkey), "%s_%d", CONFIG_ITEM_VMC_DISABLE, slot);
    configRemoveKey(configSet, gkey);
}

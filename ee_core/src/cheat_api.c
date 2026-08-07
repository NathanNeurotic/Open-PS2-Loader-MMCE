/*
 * Low-level cheat engine
 *
 * Copyright (C) 2009-2010 Mathias Lafeldt <misfire@debugon.org>
 *
 * This file is part of PS2rd, the PS2 remote debugger.
 *
 * PS2rd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PS2rd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PS2rd.  If not, see <http://www.gnu.org/licenses/>.
 *
 * $Id$
 */

#include <tamtypes.h>
#include <kernel.h>
#include <syscallnr.h>
#include "include/cheat_api.h"
#include "coreconfig.h"

/*-----------------------------------------------------------*/
/* Prebuilt PS2RD cheat-image (.img): apply at boot          */
/*-----------------------------------------------------------*/
// Apply the OPL-provided image: a header [Offset0, CountWords0, CountEntries] (Offset0 must already hold
// CountWords0 in memory as a sanity gate), then CountEntries entries of [Offset, CountWords, data...]
// written to memory. Read config->gImage DIRECTLY -- it is valid here, exactly like config->gCheatList
// (which SetupCheats also reads straight from config) -- with NO local copy, because a 4 KB local buffer
// overflows the ee_core's tight .bss/ram84 region. HARDENING: walk p against an explicit pEnd so a
// malformed/oversized image can never read past MAX_IMAGEWORDS (an OOB read). Abort on any overrun.
void LinkImage(void)
{
    USE_LOCAL_EECORE_CONFIG;
    unsigned CountEntries, CountWords, Offset;
    u32 *p = config->gImage;
    u32 *pEnd = config->gImage + MAX_IMAGEWORDS;

    Offset = *p++;
    if (!Offset)
        return;
    if (p >= pEnd)
        return;
    CountWords = *p++;
    if (*(u32 *)(Offset) != CountWords)
        return;
    if (p >= pEnd)
        return;
    CountEntries = *p++;
    while (CountEntries--) {
        if (p >= pEnd)
            return;
        Offset = *p++;
        if (p >= pEnd)
            return;
        CountWords = *p++;
        for (; CountWords--; Offset += 4) {
            if (p >= pEnd)
                return;
            *(u32 *)(Offset) = *p++;
        }
    }
}

/*---------------------------------*/
/* Setup PS2RD Cheat Engine params */
/*---------------------------------*/
void SetupCheats()
{
    USE_LOCAL_EECORE_CONFIG;
    code_t code;

    int i, j, k, nextCodeCanBeHook;
    i = 0;
    j = 0;
    k = 0;
    nextCodeCanBeHook = 1;

    while (i < MAX_CHEATLIST) {

        code.addr = config->gCheatList[i];
        code.val = config->gCheatList[i + 1];
        i += 2;

        if ((code.addr == 0) && (code.val == 0))
            break;

        if (((code.addr & 0xfe000000) == 0x90000000) && nextCodeCanBeHook == 1) {
            if (j + 2 > MAX_HOOKS * 2)
                break; // hooklist full; stop rather than overflow past its MAX_HOOKS*2 words
            hooklist[j] = code.addr & 0x01FFFFFC;
            j++;
            hooklist[j] = code.val;
            j++;
        } else {
            if (k + 2 > MAX_CODES * 2)
                break; // codelist full; stop rather than overflow past its MAX_CODES*2 words
            codelist[k] = code.addr;
            k++;
            codelist[k] = code.val;
            k++;
        }
        // Discard any false positives from being possible hooks
        if ((code.addr & 0xf0000000) == 0x40000000 || (code.addr & 0xf0000000) == 0x30000000) {
            nextCodeCanBeHook = 0;
        } else {
            nextCodeCanBeHook = 1;
        }
    }
    numhooks = j / 2;
    numcodes = k / 2;
}

/*-----------------------------------------------------*/
/* Replace SetupThread in kernel. (PS2RD Cheat Engine) */
/*-----------------------------------------------------*/
static inline void Install_HookSetupThread(void)
{
    Old_SetupThread = GetSyscallHandler(__NR_SetupThread);
    SetSyscall(__NR_SetupThread, HookSetupThread);
}

/*----------------------------------------------------*/
/* Restore original SetupThread. (PS2RD Cheat Engine) */
/*----------------------------------------------------*/
static inline void Remove_HookSetupThread(void)
{
    SetSyscall(__NR_SetupThread, Old_SetupThread);
}

/*---------------------------*/
/* Enable PS2RD Cheat Engine */
/*---------------------------*/
void EnableCheats(void)
{
    // Setup Cheats
    SetupCheats();
    // Install Hook SetupThread
    Install_HookSetupThread();
}

/*----------------------------*/
/* Disable PS2RD Cheat Engine */
/*----------------------------*/
void DisableCheats(void)
{
    // Remove Hook SetupThread
    Remove_HookSetupThread();
}

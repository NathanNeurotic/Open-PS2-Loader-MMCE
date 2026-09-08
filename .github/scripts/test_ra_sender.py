"""Exercise production RA sender functions with simulated IOP/DMA events.

Host tests cover sequencing, packet data and polling, not real SMAP timing.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'modules/network/raudp/raudp.c').read_text()


def function(name):
    start = re.search(r'^static [^\n]+\b' + name + r'\([^\n]*\)\n\{', source, re.M).start()
    end = source.index('\n}', start) + 2
    return source[start:end]


constants = '\n'.join(line for line in source.splitlines()
                      if re.match(r'#define RA_(POLL_US|IDLE_TICKS|KEEPALIVE_TICKS|HEARTBEAT_TICKS|QUIET_US)\b', line))
fields = source[source.index('struct ra_field\n'):source.index('/* ---- Formatting helpers')]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "modules/network/common/ra_snap.h"
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned iop_sys_clock_t;
#define USE_SMAP_REGS
#define SMAP_REG8(x) 0
#define RA_PAYLOAD 1472
#define RA_HDR_LEN 42
#define RA_FRAME_LEN (RA_HDR_LEN + RA_PAYLOAD)
#define RA_OFF(id) (ra_fields[id].off)
static u8 ra_frame[RA_FRAME_LEN], *ra_payload = ra_frame + RA_HDR_LEN;
static u8 ra_stage[RA_SNAP_MAX_BYTES] __attribute__((aligned(4)));
static u8 storage[RA_SNAP_TOTAL] __attribute__((aligned(4)));
static volatile struct ra_snap *ra_snap = (void *)storage;
static char ra_game_id[16] = "SLUS_210.65";
static u32 ra_seq, ra_fail, ra_us, ra_us_max, ra_rxq, ra_skip, ra_snap_bad, ra_sent_sq;
static int ra_err, ra_sent_any, ra_rx_in_game = 1;
static u32 ra_disc_us;
static unsigned packets, failed_packet, advance_packet, busy, drains, polls, heartbeats;
static unsigned tick, limit, produce, quiet;
static jmp_buf finished;
static void ra_fmt(u8 *, u32, int);
static void ra_fmt_err(u8 *, int);
static void snapshot(u32 seq, u32 bytes) {
    memset(storage, 0, sizeof(storage));
    ra_snap = (void *)storage;
    ra_snap->magic = RA_SNAP_MAGIC;
    ra_snap->seq = seq;
    ra_snap->bytes = bytes;
    ra_snap->count = bytes;
    memcpy((void *)ra_snap->game_id, ra_game_id, 16);
    memset(storage + RA_SNAP_HDR, (u8)(seq % 1000000), bytes);
    *(u32 *)(storage + RA_SNAP_TRAILER_OFF(bytes)) = seq;
}
static int ra_tx_busy(void) { return busy; }
static void GetSystemTime(iop_sys_clock_t *t) { *t = 0; }
static u32 ra_usec_delta(iop_sys_clock_t *a, iop_sys_clock_t *b) { (void)a; (void)b; return 1; }
static int SMAPSendPacket(const void *, unsigned);
static int ra_discover(void) { return 1; }
static void ra_frame_init(void);
static void ra_drain_rx(void) { drains++; }
static void ra_poll_pc(void) { polls++; }
static void ra_heartbeat(void) { heartbeats++; }
static void DelayThread(unsigned);
'''
stubs = r'''
static unsigned field(int id) {
    unsigned n = 0;
    for (int i = 0; i < ra_fields[id].width; i++)
        n = n * 10 + ra_payload[RA_OFF(id) + i] - '0';
    return n;
}
static int SMAPSendPacket(const void *p, unsigned len) {
    assert(p == ra_frame && len == RA_FRAME_LEN);
    unsigned sq = field(RA_F_SQ), nb = field(RA_F_VB);
    for (unsigned i = 0; i < nb; i++)
        assert(ra_payload[ra_head_len + i] == (u8)sq);
    packets++;
    if (packets == advance_packet) snapshot(sq + 1, ra_snap->bytes);
    return packets == failed_packet ? -1 : 1;
}
static void ra_frame_init(void) { ra_head_build(); }
static void DelayThread(unsigned us) {
    if (us != RA_POLL_US) {
        assert(us == RA_QUIET_US - ra_disc_us);
        quiet++;
        return;
    }
    if (++tick == limit) longjmp(finished, 1);
    if (produce && tick % 4 == 0) snapshot(ra_snap->seq + 1, 8);
}
static void reset(void) {
    ra_seq = ra_fail = ra_us = ra_us_max = ra_rxq = ra_skip = ra_snap_bad = ra_sent_sq = 0;
    ra_err = ra_sent_any = 0;
    packets = failed_packet = advance_packet = busy = drains = polls = heartbeats = 0;
    tick = limit = produce = quiet = 0;
    ra_disc_us = RA_QUIET_US;
    ra_rx_in_game = 1;
    snapshot(1, 8);
    ra_head_build();
}
static void loop(unsigned count) {
    limit = count;
    if (!setjmp(finished)) ra_thread(NULL);
}
int main(void) {
    /* Every source/destination alignment, boundary length and untouched byte. */
    u8 src[RA_SNAP_MAX_BYTES + 8], dst[RA_SNAP_MAX_BYTES + 8];
    for (unsigned i = 0; i < sizeof(src); i++) src[i] = (u8)(i * 17);
    for (unsigned a = 0; a < 4; a++) for (unsigned b = 0; b < 4; b++)
        for (unsigned n = 0; n <= RA_SNAP_MAX_BYTES; n++) {
            memset(dst, 0xCC, sizeof(dst));
            ra_copy(dst + b, src + a, n);
            assert(!memcmp(dst + b, src + a, n));
            for (unsigned i = 0; i < b; i++) assert(dst[i] == 0xCC);
            for (unsigned i = b + n; i < sizeof(dst); i++) assert(dst[i] == 0xCC);
        }
    puts("PASS: aligned/unaligned copies and write boundaries");
    reset();
    busy = 1;
    ra_send_one();
    assert(!packets && !ra_seq && ra_snap_pending() == 1 && ra_skip == 1);
    busy = 0;
    *(u32 *)(storage + RA_SNAP_TRAILER_OFF(8)) = 0;
    ra_send_one();
    assert(!packets && !ra_seq && ra_snap_pending() == 1 && ra_snap_bad == 1);
    *(u32 *)(storage + RA_SNAP_TRAILER_OFF(8)) = 1;
    ra_send_one();
    assert(packets == 1 && ra_sent_sq == 1 && ra_snap_pending() == 0);
    puts("PASS: busy and torn snapshots remain pending, then send successfully");
    for (unsigned fail = 1; fail <= RA_SNAP_PARTS; fail++) {
        reset(); snapshot(41, RA_SNAP_MAX_BYTES); failed_packet = fail;
        ra_send_one();
        assert(ra_fail == 1 && !ra_sent_any && ra_snap_pending() == 1);
        failed_packet = 0;
        ra_send_one();
        assert(packets == 2 * RA_SNAP_PARTS && ra_sent_sq == 41 && ra_snap_pending() == 0);
    }
    puts("PASS: failure of any multipart packet leaves the snapshot retryable");
    reset(); snapshot(41, RA_SNAP_MAX_BYTES); advance_packet = 1;
    ra_send_one();
    assert(ra_sent_sq == 41 && ra_snap->seq == 42 && ra_snap_pending() == 1);
    ra_send_one();
    assert(ra_sent_sq == 42 && ra_snap_pending() == 0);
    snapshot(UINT32_MAX, 8); ra_send_one(); snapshot(0, 8);
    assert(ra_snap_pending() == 1);
    ra_send_one(); assert(ra_snap_pending() == 0);
    puts("PASS: DMA advancement during multipart send and sequence wraparound");
    reset(); produce = 1; loop(2500);
    assert(packets == 625 && drains == 625 && polls == 625 && heartbeats == 1);
    reset(); ra_disc_us = 0; ra_rx_in_game = 0; loop(501);
    assert(packets == 3 && !drains && !polls && quiet == 1);
    reset(); ra_snap = NULL; loop(16);
    assert(packets == 4 && !ra_sent_any);
    reset(); busy = 1; loop(20);
    assert(!packets && ra_skip == 20);
    puts("PASS: new-only sending, idle keepalive, RX guard, quiet period and header-only cadence");
}
'''
program = (prefix + constants + '\n' + fields + '\n' +
           '\n'.join(function(n) for n in ('ra_fmt', 'ra_fmt_err', 'ra_copy',
                                         'ra_send_one', 'ra_snap_pending', 'ra_thread')) + stubs)
with tempfile.TemporaryDirectory(prefix='ra-sender-') as work:
    path = Path(work) / 'sender.c'
    exe = Path(work) / 'sender'
    path.write_text(program)
    # IOP pointers are 32-bit; on a 64-bit host the copy helper only examines
    # their low alignment bits. Suppress that one target-size warning.
    subprocess.run(['gcc', '-std=gnu99', '-O2', '-fno-strict-aliasing', '-Wall', '-Wextra',
                    '-Werror', '-Wno-pointer-to-int-cast', '-I', str(root),
                    str(path), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

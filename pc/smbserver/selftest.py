#!/usr/bin/env python3
# Self-test for smbserver_opl.py -- a raw SMB1 client that mimics Open-PS2-Loader's exact wire
# sequence (share-level + plaintext guest, no NTLM challenge), so it validates the real OPL path
# that off-the-shelf clients (pysmb/smbclient) can't, including the high-risk bits:
#   - NEGOTIATE response WordCount == 17 and SecurityMode == 0x00 (OPL hard-checks these)
#   - READ_ANDX DataOffset == 59 with byte-exact payload at the right place
#   - FIND_FIRST2 / FIND_NEXT2 directory listing the OPL menu depends on
# Run:  python selftest.py    (starts the server itself on a scratch port + temp share)
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MAGIC = b"\xffSMB"
ok = 0
fail = 0


def check(name, cond, extra=""):
    global ok, fail
    if cond:
        ok += 1
        print("  PASS", name)
    else:
        fail += 1
        print("  FAIL", name, extra)


def nbss_send(s, msg):
    s.sendall(b"\x00" + len(msg).to_bytes(3, "big") + msg)


def nbss_recv(s):
    h = b""
    while len(h) < 4:
        c = s.recv(4 - len(h))
        if not c:
            raise IOError("eof")
        h += c
    n = int.from_bytes(h[1:4], "big")
    body = b""
    while len(body) < n:
        c = s.recv(n - len(body))
        if not c:
            raise IOError("eof")
        body += c
    return body


def hdr(cmd, tid=0, uid=0, mid=1):
    h = bytearray(32)
    h[0:4] = MAGIC
    h[4] = cmd
    h[9] = 0x00
    struct.pack_into("<H", h, 10, 0x4001)
    struct.pack_into("<H", h, 24, tid)
    struct.pack_into("<H", h, 28, uid)
    struct.pack_into("<H", h, 30, mid)
    return bytes(h)


def body(params, data):
    return bytes([len(params) // 2]) + params + struct.pack("<H", len(data)) + data


def parse(msg):
    cmd = msg[4]
    status = msg[5] | (struct.unpack_from("<H", msg, 7)[0] << 16)
    tid = struct.unpack_from("<H", msg, 24)[0]
    uid = struct.unpack_from("<H", msg, 28)[0]
    wc = msg[32]
    params = msg[33:33 + wc * 2]
    bc = struct.unpack_from("<H", msg, 33 + wc * 2)[0]
    data = msg[35 + wc * 2:35 + wc * 2 + bc]
    return cmd, status, tid, uid, wc, params, data, bc


def run(port, share, isofile, isodata, tmpdir):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)

    # --- NEGOTIATE ---
    nbss_send(s, hdr(0x72) + body(b"", b"\x02NT LM 0.12\x00"))
    cmd, status, tid, uid, wc, params, data, bc = parse(nbss_recv(s))
    check("NEGOTIATE status success", status == 0, hex(status))
    check("NEGOTIATE WordCount == 17", wc == 17, "got %d" % wc)
    sec_mode = params[2]
    check("NEGOTIATE SecurityMode == 0 (share+plaintext)", sec_mode == 0, hex(sec_mode))
    dialect_idx = struct.unpack_from("<H", params, 0)[0]
    check("NEGOTIATE DialectIndex == 0", dialect_idx == 0, str(dialect_idx))

    # --- SESSION_SETUP_ANDX (no password / guest) ---
    # 13 words: AndX(1)Res(1)Off(2) MaxBuf(2) MaxMpx(2) VC(2) SessKey(4) AnsiLen(2) UniLen(2) Res(4) Caps(4)
    ss_params = struct.pack("<BBHHHHIHHII", 0xFF, 0, 0, 0x2000, 1, 1, 0, 0, 0, 0, 0)
    ss_data = b"\x00PlayStation 2\x00WORKGROUP\x00OPL\x00"
    nbss_send(s, hdr(0x73) + body(ss_params, ss_data))
    cmd, status, tid, uid, wc, params, data, bc = parse(nbss_recv(s))
    check("SESSION_SETUP status success", status == 0, hex(status))
    check("SESSION_SETUP returns non-zero UID", uid != 0, str(uid))
    sess_uid = uid

    # --- TREE_CONNECT_ANDX  \\x\<share>  (share-level: 1-byte password) ---
    path = ("\\\\127.0.0.1\\" + share).encode("ascii") + b"\x00"
    tc_params = struct.pack("<BBHHH", 0xFF, 0, 0, 0, 1)  # AndX, Flags, PasswordLength=1
    tc_data = b"\x00" + path + b"?????\x00"
    nbss_send(s, hdr(0x75, uid=sess_uid) + body(tc_params, tc_data))
    cmd, status, tid, uid, wc, params, data, bc = parse(nbss_recv(s))
    check("TREE_CONNECT status success", status == 0, hex(status))
    check("TREE_CONNECT returns non-zero TID", tid != 0, str(tid))
    sess_tid = tid

    # --- TRANS2 FIND_FIRST2 (list the share root) ---
    t2p = struct.pack("<HHHHIH", 0x16, 1, 0x0006, 0x0104, 0, 0) + b"\\*\x00"
    # Transaction2 request: 15 setup words; place params right after the fixed area.
    setup = struct.pack("<H", 0x0001)  # FIND_FIRST2
    fixed = struct.pack(
        "<HHHHBBHIH HH HH BB",
        len(t2p), 0, 1024, 0, 0, 0, 0, 0, 0,  # TPC,TDC,MaxPC,MaxDC,MaxSetup,Res,Flags,Timeout,Res2
        0, 0,  # ParameterCount, ParameterOffset (filled below)
        0, 0,  # DataCount, DataOffset
        1, 0,  # SetupCount=1, Reserved3
    )
    # recompute offsets: header(32)+WC(1)+params(len(fixed)+len(setup))+BC(2)+name pad
    wc_words = (len(fixed) + len(setup)) // 2
    name = b"\x00"  # trans name (empty)
    param_off = 32 + 1 + len(fixed) + len(setup) + 2 + len(name)
    # rebuild fixed with correct ParameterCount/Offset + DataCount/Offset(0)
    fixed = struct.pack(
        "<HHHHBBHIHHHHHBB",
        len(t2p), 0, 1024, 0, 0, 0, 0, 0, 0,
        len(t2p), param_off, 0, 0, 1, 0,
    )
    msg = hdr(0x32, tid=sess_tid, uid=sess_uid)
    msg += bytes([wc_words]) + fixed + setup + struct.pack("<H", len(name) + len(t2p)) + name + t2p
    nbss_send(s, msg)
    cmd, status, tid, uid, wc, params, data, bc = parse(nbss_recv(s))
    check("FIND_FIRST2 status success", status == 0, hex(status))
    # trans2 response: re-read ParameterOffset/DataOffset from its own fixed words
    reply = nbss_last[0]
    p_off = struct.unpack_from("<H", reply, 33 + 4 * 2)[0]
    p_cnt = struct.unpack_from("<H", reply, 33 + 3 * 2)[0]
    d_off = struct.unpack_from("<H", reply, 33 + 7 * 2)[0]
    d_cnt = struct.unpack_from("<H", reply, 33 + 6 * 2)[0]
    tparams = reply[p_off:p_off + p_cnt]
    tdata = reply[d_off:d_off + d_cnt]
    sid = struct.unpack_from("<H", tparams, 0)[0]
    search_count = struct.unpack_from("<H", tparams, 2)[0]
    check("FIND_FIRST2 returned an entry", search_count == 1, str(search_count))
    namelen = struct.unpack_from("<I", tdata, 60)[0]
    fname = tdata[94:94 + namelen].decode("ascii", "ignore")
    check("FIND_FIRST2 first entry is '.'", fname == ".", repr(fname))

    # loop FIND_NEXT2 until we see our iso
    found_iso = False
    for _ in range(50):
        np = struct.pack("<HHHIH", sid, 1, 0x0104, 0, 0x000E) + b"\x00"
        setup2 = struct.pack("<H", 0x0002)  # FIND_NEXT2
        fixed2 = struct.pack("<HHHHBBHIHHHHHBB", len(np), 0, 1024, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0)
        wcw = (len(fixed2) + len(setup2)) // 2
        p_off2 = 32 + 1 + len(fixed2) + len(setup2) + 2 + 1
        fixed2 = struct.pack("<HHHHBBHIHHHHHBB", len(np), 0, 1024, 0, 0, 0, 0, 0, 0, len(np), p_off2, 0, 0, 1, 0)
        m2 = hdr(0x32, tid=sess_tid, uid=sess_uid)
        m2 += bytes([wcw]) + fixed2 + setup2 + struct.pack("<H", 1 + len(np)) + b"\x00" + np
        nbss_send(s, m2)
        reply = nbss_recv(s)
        nbss_last[0] = reply
        _c, _st, _t, _u, _wc, _p, _d, _bc = parse(reply)
        if _st != 0:
            break
        po = struct.unpack_from("<H", reply, 33 + 4 * 2)[0]
        pc = struct.unpack_from("<H", reply, 33 + 3 * 2)[0]
        do = struct.unpack_from("<H", reply, 33 + 7 * 2)[0]
        dc = struct.unpack_from("<H", reply, 33 + 6 * 2)[0]
        tp = reply[po:po + pc]
        td = reply[do:do + dc]
        sc = struct.unpack_from("<H", tp, 0)[0]
        if sc == 0 or dc == 0:
            break
        nl = struct.unpack_from("<I", td, 60)[0]
        nm = td[94:94 + nl].decode("ascii", "ignore")
        if nm == isofile:
            found_iso = True
        eos = struct.unpack_from("<H", tp, 2)[0]
        if eos:
            break
    check("FIND listing includes the iso", found_iso, isofile)

    # --- OPEN_ANDX the iso ---
    op_params = struct.pack("<BBHHHHHHHI", 0xFF, 0, 0, 0, 0, 1, 0, 0, 0, 0)
    nbss_send(s, hdr(0x2D, tid=sess_tid, uid=sess_uid) + body(op_params, isofile.encode() + b"\x00"))
    reply = nbss_recv(s)
    nbss_last[0] = reply
    cmd, status, tid, uid, wc, params, data, bc = parse(reply)
    check("OPEN_ANDX status success", status == 0, hex(status))
    fid = struct.unpack_from("<H", params, 4)[0]
    check("OPEN_ANDX returns non-zero FID", fid != 0, str(fid))

    # --- READ_ANDX: the critical path. Request fields per ReadAndXRequest_t. ---
    def read_at(offset, count):
        rp = struct.pack("<BBHHIHHIHI", 0xFF, 0, 0, fid, offset & 0xFFFFFFFF, count & 0xFFFF, 0, 0, 0, offset >> 32)
        nbss_send(s, hdr(0x2E, tid=sess_tid, uid=sess_uid) + body(rp, b""))
        reply = nbss_recv(s)
        c, st, t, u, w, p, d, b = parse(reply)
        do = struct.unpack_from("<H", p, 12)[0]      # DataOffset field
        dll = struct.unpack_from("<H", p, 10)[0]     # DataLengthLow
        dlh = struct.unpack_from("<I", p, 14)[0]     # DataLengthHigh
        dlen = dll | (dlh << 16)
        payload = reply[do:do + dlen]
        return st, do, dlen, payload

    st, do, dlen, payload = read_at(0, 4096)
    check("READ_ANDX status success", st == 0, hex(st))
    check("READ_ANDX DataOffset == 59", do == 59, "got %d" % do)
    check("READ_ANDX returned 4096 bytes", dlen == 4096, str(dlen))
    check("READ_ANDX payload byte-exact @0", payload == isodata[:4096], "len %d" % len(payload))

    st, do, dlen, payload = read_at(100000, 4096)
    check("READ_ANDX honors 64-bit offset", payload == isodata[100000:104096], "len %d" % len(payload))

    # --- CLOSE ---
    nbss_send(s, hdr(0x04, tid=sess_tid, uid=sess_uid) + body(struct.pack("<HI", fid, 0), b""))
    cmd, status, *_ = parse(nbss_recv(s))
    check("CLOSE status success", status == 0, hex(status))

    # --- ECHO ---
    nbss_send(s, hdr(0x2B, tid=sess_tid, uid=sess_uid) + body(struct.pack("<H", 1), b"ALIVE ECHO TEST"))
    cmd, status, tid, uid, wc, params, data, bc = parse(nbss_recv(s))
    check("ECHO bounces payload", data == b"ALIVE ECHO TEST", repr(data))

    # --- QUERY_INFORMATION_DISK ---
    nbss_send(s, hdr(0x80, tid=sess_tid, uid=sess_uid) + body(b"", b""))
    cmd, status, *_ = parse(nbss_recv(s))
    check("QUERY_INFORMATION_DISK success", status == 0, hex(status))

    # --- NT_CREATE_ANDX (smbman cover-art / cfg open) ---
    nm = b"game.cfg"
    nc_params = struct.pack("<BBHBHIIIQIIIIIB", 0xFF, 0, 0, 0, len(nm), 0, 0, 0, 0, 0, 0, 1, 0, 0, 0)
    nbss_send(s, hdr(0xA2, tid=sess_tid, uid=sess_uid) + body(nc_params, nm + b"\x00"))
    cmd, status, tid, uid, wc, params, data, bc = parse(nbss_recv(s))
    check("NT_CREATE_ANDX status success", status == 0, hex(status))
    ntfid = struct.unpack_from("<H", params, 5)[0]
    check("NT_CREATE_ANDX returns FID", ntfid != 0, str(ntfid))

    # --- TRANS2 QUERY_PATH_INFORMATION (stat: basic + standard info) ---
    def query_path(level, pathname):
        qp = struct.pack("<HI", level, 0) + pathname.encode() + b"\x00"
        setupq = struct.pack("<H", 0x0005)
        poff = 32 + 1 + 28 + len(setupq) + 2 + 1  # 28-byte trans2 fixed block
        fixedq = struct.pack("<HHHHBBHIHHHHHBB", len(qp), 0, 1024, 0, 0, 0, 0, 0, 0, len(qp), poff, 0, 0, 1, 0)
        wcw = (len(fixedq) + len(setupq)) // 2
        m = hdr(0x32, tid=sess_tid, uid=sess_uid)
        m += bytes([wcw]) + fixedq + setupq + struct.pack("<H", 1 + len(qp)) + b"\x00" + qp
        nbss_send(s, m)
        reply = nbss_recv(s)
        c, st, t, u, w, p, d, b = parse(reply)
        do = struct.unpack_from("<H", reply, 33 + 7 * 2)[0]
        dc = struct.unpack_from("<H", reply, 33 + 6 * 2)[0]
        return st, reply[do:do + dc]

    st, td = query_path(0x0101, "\\hello.iso")
    check("QUERY_PATH basic-info success", st == 0, hex(st))
    check("QUERY_PATH basic attr is file (0x80)", len(td) >= 36 and struct.unpack_from("<I", td, 32)[0] == 0x80)
    st, td = query_path(0x0102, "\\hello.iso")
    check("QUERY_PATH standard-info success", st == 0, hex(st))
    check("QUERY_PATH standard EOF == filesize", len(td) >= 16 and struct.unpack_from("<Q", td, 8)[0] == len(isodata))

    # --- WRITE_ANDX (writable by default; OPL config + VMC-on-SMB saves) ---
    op = struct.pack("<BBHHHHHHHI", 0xFF, 0, 0, 0, 0, 1, 0, 0, 0, 0)
    nbss_send(s, hdr(0x2D, tid=sess_tid, uid=sess_uid) + body(op, b"game.cfg\x00"))
    cmd, status, tid, uid, wc, params, data, bc = parse(nbss_recv(s))
    wfid = struct.unpack_from("<H", params, 4)[0]
    payload = b"OPLWRITE_OK!"
    data_off = 32 + 1 + 28 + 2  # payload immediately after ByteCount in a 14-word WRITE_ANDX request
    wp = struct.pack("<BBHHIIHHHHHI", 0xFF, 0, 0, wfid, 0, 0, 0, 0, len(payload) >> 16, len(payload) & 0xFFFF, data_off, 0)
    nbss_send(s, hdr(0x2F, tid=sess_tid, uid=sess_uid) + bytes([14]) + wp + struct.pack("<H", len(payload)) + payload)
    cmd, status, tid, uid, wc, params, data, bc = parse(nbss_recv(s))
    check("WRITE_ANDX status success (writable default)", status == 0, hex(status))
    wcount = struct.unpack_from("<H", params, 4)[0] if len(params) >= 6 else 0
    check("WRITE_ANDX reports bytes written", wcount == len(payload), str(wcount))
    nbss_send(s, hdr(0x04, tid=sess_tid, uid=sess_uid) + body(struct.pack("<HI", wfid, 0), b""))
    parse(nbss_recv(s))
    with open(os.path.join(tmpdir, "game.cfg"), "rb") as f:
        disk = f.read()
    check("WRITE_ANDX bytes actually hit disk", disk[:len(payload)] == payload, repr(disk[:16]))

    s.close()


nbss_last = [b""]
_orig_recv = nbss_recv


def nbss_recv(s):  # noqa: F811  -- wrap to stash the last reply for trans2 offset re-reads
    r = _orig_recv(s)
    nbss_last[0] = r
    return r


def main():
    tmp = tempfile.mkdtemp(prefix="oplsmbtest_")
    os.makedirs(os.path.join(tmp, "SUBDIR"), exist_ok=True)
    isodata = bytes(range(256)) * 800  # 204800 bytes, deterministic
    isofile = "hello.iso"
    with open(os.path.join(tmp, isofile), "wb") as f:
        f.write(isodata)
    with open(os.path.join(tmp, "game.cfg"), "w") as f:
        f.write("config")

    port = 14455
    proc = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "smbserver_opl.py"), "--share", "games=" + tmp,
         "--port", str(port), "-v"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    # wait for bind
    deadline = time.time() + 8
    while time.time() < deadline:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.3).close()
            break
        except OSError:
            time.sleep(0.1)
    try:
        run(port, "games", isofile, isodata, tmp)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    print("\n  %d passed, %d failed" % (ok, fail))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())

# HTTP catalog and range conformance

Phase 1 of [docs/HTTP-INTEGRATION-PLAN.md](../../docs/HTTP-INTEGRATION-PLAN.md): the contract the
console client must implement, written down and made executable *before* any of it is ported to C.

Nothing here ships to a PlayStation 2. These are host-side tools.

## What is here

| File | What it is |
| --- | --- |
| `catalog_reference.py` | The reference `games.csv` parser. Section 4 of the plan, executable. |
| `fixtures/*.csv` | The catalog cases the parser must get right. |
| `fixtures/expected.json` | The expected parse of every fixture. **This is the contract.** |
| `http_conformance.py` | Checks a live HTTP server against the range profile. |

## Checking the parser

```bash
python pc/http/catalog_reference.py --selftest
```

Every fixture is parsed in three line-ending forms — LF, CRLF, and no final newline — and diffed
against `expected.json`.

The variants are generated at read time on purpose. `.gitattributes` has `* text=auto`, so a
committed CRLF fixture would be stored as LF and check out as LF on Linux CI: the CRLF case would
silently test nothing. Generating the variants means git normalisation cannot lie to us.

`--emit-expected` regenerates `expected.json` from the current parser. **Read the diff.** The file
is only worth something because a human agreed with it; regenerating it to make a failing test pass
converts the contract into a description of whatever the code currently does.

When the C parser in `src/httpsupport.c` exists it must agree with `expected.json` on every fixture.
If the two disagree, this directory is right and the C is wrong — unless the disagreement is
deliberate, in which case change the fixture, the plan and the C in one commit.

## Checking a server

```bash
python pc/http/http_conformance.py http://192.168.1.10:1100/
python pc/http/http_conformance.py http://192.168.1.10:1100/ --iso "DVD/Game.iso" \
                                   --verify-against /path/to/local/Game.iso
```

It asks what the PS2 will ask, in the order it will ask it, over one persistent connection, and
fails on the things that would corrupt a disc read rather than the things that merely look untidy.

**Docmine17's unmodified `pc/http_server.py` must pass.** It does, as of donor commit
`6fced11a` — 14 passed, 0 failed, with the 4 GiB read skipped for want of a large enough test image.
That run also confirmed two behaviours the client has to accommodate: his server answers an
out-of-range request with **416 and no `Content-Range` header**, and it sends `Accept-Ranges` on
error responses too, so neither can be used as a correctness signal.

### What it cannot tell you

A server that returns the wrong bytes behind *correct* headers is invisible to every header check —
this is exactly the failure the donor's status-code-only validation
(`modules/iopcore/cdvdman/http.c:250-269`) cannot see, and it is why the plan requires validating
`Content-Range` and `Content-Length` against the request.

The content-agreement check cross-reads the same region two ways, which catches non-uniform
corruption. It **cannot** catch a server that shifts every read by the same amount: such a server is
self-consistent by construction. Only `--verify-against` against a known-good local copy, or booting
the game on hardware, closes that gap. A green run without `--verify-against` is not proof the bytes
are right.

The harness was checked against a deliberately lying server (206 with correct headers over
sector-shifted data) to confirm it fails when it should. Both limitations above were found that way,
not reasoned about.

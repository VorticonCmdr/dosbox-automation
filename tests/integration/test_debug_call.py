"""POST /api/v1/debug/call against a live headless instance.

Small hand-assembled real-mode routines are written into a DOS memory block,
the debugger is paused, and each routine is called through the route. Checks
the result registers and memory diffs, that the machine is exactly as before
afterwards, that DOS answers the routine's INT 21h, that port I/O is
reported as unrestored, and that a routine that never returns fails cleanly.
Needs a binary built with the debugger (build-debugger/).
"""
import base64
import time

import pytest

# shr cx,1 ; add ax,cx ; shr dx,1 ; add bx,dx ; inc word [0x2000] ; retf / ret
BOX_RETF = bytes.fromhex("D1E901C8D1EA01D3FF060020CB")
BOX_RET = bytes.fromhex("D1E901C8D1EA01D3FF060020C3")
# mov ah,2Ch ; int 21h ; retf   (DOS get time)
GETTIME_RETF = bytes.fromhex("B42CCD21CB")
# mov al,5 ; out 80h,al ; retf
OUT_RETF = bytes.fromhex("B005E680CB")
# jmp $
SPIN = bytes.fromhex("EBFE")
# push bp ; mov bp,sp ; mov ax,[bp+6] ; pop bp ; retf   (first stack argument of a far call)
STACK_ARG_RETF = bytes.fromhex("558BEC8B46065DCB")
# mov ax,40h ; mov es,ax ; mov bx,es:[6Ch] ; wait: cmp bx,es:[6Ch] ; je wait ; retf
# (waits for the BIOS tick count to change: only the timer IRQ does that)
WAIT_TICK_RETF = bytes.fromhex("B840008EC0268B1E6C00263B1E6C0074F9CB")
# mov ah,3Bh ; mov dx,0800h ; int 21h ; retf   (DOS chdir to DS:0800)
CHDIR_RETF = bytes.fromhex("B43BBA0008CD21CB")

OFF_BOX_RETF, OFF_BOX_RET, OFF_GETTIME, OFF_OUT, OFF_SPIN = 0x100, 0x200, 0x300, 0x400, 0x500
OFF_STACK_ARG, OFF_CHDIR, OFF_PATH, OFF_WAIT_TICK = 0x600, 0x700, 0x800, 0x900
COUNTER = 0x2000
BLOCK_BYTES = 0x3000


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode()


@pytest.fixture(scope="module")
def routines(dosbox):
    if not dosbox.dosbox_info().json().get("features", {}).get("debug_call"):
        pytest.skip("binary built without the debugger")
    r = dosbox._post("/api/v1/memory/allocate", json={"size": BLOCK_BYTES, "area": "CONV"})
    assert r.status_code == 200, r.text
    base = r.json()["addr"]
    assert base % 16 == 0
    for off, code in ((OFF_BOX_RETF, BOX_RETF), (OFF_BOX_RET, BOX_RET), (OFF_GETTIME, GETTIME_RETF),
                      (OFF_OUT, OUT_RETF), (OFF_SPIN, SPIN), (OFF_STACK_ARG, STACK_ARG_RETF),
                      (OFF_CHDIR, CHDIR_RETF), (OFF_PATH, b"\\\0"), (OFF_WAIT_TICK, WAIT_TICK_RETF)):
        w = dosbox._put(f"/api/v1/memory/{base + off}", json={"data": b64(code)})
        assert w.status_code == 200, w.text
    zero = dosbox._put(f"/api/v1/memory/{base + COUNTER}", json={"data": b64(b"\0\0")})
    assert zero.status_code == 200
    p = dosbox._post("/api/v1/debug/pause")
    assert p.status_code == 200, p.text
    for _ in range(50):
        if dosbox._get("/api/v1/debug/status").json().get("debugging"):
            break
        time.sleep(0.1)
    yield base >> 4, base
    dosbox._post("/api/v1/debug/continue")


def call(dosbox, body, timeout=30):
    r = dosbox.session.post(dosbox._url("/api/v1/debug/call"), json=body, timeout=timeout)
    return r


def counter_range(base):
    return [{"space": "ram", "start": base + COUNTER, "end": base + COUNTER + 2}]


def read_mem(dosbox, addr, n):
    r = dosbox._get(f"/api/v1/memory/{addr}/{n}", headers={"Accept": "application/json"})
    assert r.status_code == 200, r.text
    return base64.b64decode(r.json()["memory"]["data"])


def test_far16_call_returns_results_and_diffs(dosbox, routines):
    seg, base = routines
    r = call(dosbox, {"kind": "far16", "segment": seg, "offset": OFF_BOX_RETF,
                      "registers": {"eax": 10, "ebx": 20, "ecx": 6, "edx": 9, "ds": seg},
                      "ranges": counter_range(base)})
    assert r.status_code == 200, r.text
    (res,) = r.json()["results"]
    assert res["returned"] and res["error"] is None
    assert res["registers"]["eax"] & 0xFFFF == 13 and res["registers"]["ebx"] & 0xFFFF == 24
    (rng,) = res["ranges"]
    assert rng["changed_bytes"] == 1
    assert rng["diffs"] == [{"address": base + COUNTER, "data": b64(b"\x01")}]
    assert res["ports"] == [] and res["unrestored"] == []


def test_machine_state_is_restored(dosbox, routines):
    seg, base = routines
    before = dosbox._get("/api/v1/cpu/state").json()
    r = call(dosbox, {"kind": "far16", "segment": seg, "offset": OFF_BOX_RETF,
                      "registers": {"eax": 1, "ecx": 2, "ds": seg}})
    assert r.json()["results"][0]["returned"]
    after = dosbox._get("/api/v1/cpu/state").json()
    assert before == after
    assert read_mem(dosbox, base + COUNTER, 2) == b"\0\0"


def test_near16_call_with_stub(dosbox, routines):
    seg, base = routines
    r = call(dosbox, {"kind": "near16", "segment": seg, "offset": OFF_BOX_RET, "stub_offset": 0x0FF0,
                      "registers": {"eax": 100, "ebx": 0, "ecx": 50, "edx": 0, "ds": seg},
                      "ranges": [{"start": base, "end": base + BLOCK_BYTES}]})
    (res,) = r.json()["results"]
    assert res["returned"], res
    assert res["registers"]["eax"] & 0xFFFF == 125
    # only the counter changed: the return stub is not part of the diff
    assert [d["address"] for d in res["ranges"][0]["diffs"]] == [base + COUNTER]


def test_batch_cases_each_start_from_the_snapshot(dosbox, routines):
    seg, base = routines
    cases = [{"registers": {"eax": x, "ecx": 2 * x, "ds": seg}} for x in (1, 2, 3)]
    r = call(dosbox, {"kind": "far16", "segment": seg, "offset": OFF_BOX_RETF, "cases": cases,
                      "ranges": counter_range(base)})
    results = r.json()["results"]
    assert [x["registers"]["eax"] & 0xFFFF for x in results] == [2, 4, 6]
    # the counter is 0 -> 1 in every case: each case starts from the snapshot
    assert all(x["ranges"][0]["diffs"] == [{"address": base + COUNTER, "data": b64(b"\x01")}]
               for x in results)


def test_memory_inputs(dosbox, routines):
    seg, base = routines
    r = call(dosbox, {"kind": "far16", "segment": seg, "offset": OFF_BOX_RETF,
                      "registers": {"ds": seg},
                      "memory": [{"address": base + COUNTER, "data": b64(b"\x41\x00")}],
                      "ranges": counter_range(base)})
    (res,) = r.json()["results"]
    assert res["ranges"][0]["diffs"] == [{"address": base + COUNTER, "data": b64(b"\x42")}]
    assert read_mem(dosbox, base + COUNTER, 2) == b"\0\0"


def test_dos_answers_the_routines_interrupt(dosbox, routines):
    seg, _ = routines
    (res,) = call(dosbox, {"kind": "far16", "segment": seg, "offset": OFF_GETTIME}).json()["results"]
    assert res["returned"], res
    assert {"number": 0x21, "ah": 0x2C, "kind": "software", "count": 1} in res["interrupts"]
    assert res["unrestored"] == []


def test_port_io_is_reported_as_unrestored(dosbox, routines):
    seg, _ = routines
    (res,) = call(dosbox, {"kind": "far16", "segment": seg, "offset": OFF_OUT}).json()["results"]
    assert res["returned"]
    assert res["ports"] == [{"port": 0x80, "reads": 0, "writes": 1}]
    assert any("port I/O" in u for u in res["unrestored"])


def test_routine_that_never_returns_fails_cleanly(dosbox, routines):
    seg, base = routines
    before = dosbox._get("/api/v1/cpu/state").json()
    (res,) = call(dosbox, {"kind": "far16", "segment": seg, "offset": OFF_SPIN,
                           "max_cycles": 200000}).json()["results"]
    assert not res["returned"] and "did not return" in res["error"]
    assert dosbox._get("/api/v1/cpu/state").json() == before
    # and the next call still works
    ok = call(dosbox, {"kind": "far16", "segment": seg, "offset": OFF_BOX_RETF,
                       "registers": {"ds": seg}}).json()["results"][0]
    assert ok["returned"]


def test_return_mode_replace_keeps_stack_arguments_in_place(dosbox, routines):
    # Paused at a routine's first instruction, the caller's far return
    # address is already at SS:SP with the arguments above it. Fake that
    # frame with memory inputs: return FFFF:AAAA, first argument 1234h.
    seg, _ = routines
    regs = dosbox._get("/api/v1/cpu/state").json()["registers"]
    sp_linear = (regs["ss"] << 4) + (regs["esp"] & 0xFFFF)
    frame = [{"address": sp_linear, "data": b64(bytes.fromhex("AAAAFFFF3412"))}]
    body = {"kind": "far16", "segment": seg, "offset": OFF_STACK_ARG, "memory": frame}
    (replaced,) = call(dosbox, {**body, "return": "replace"}).json()["results"]
    assert replaced["returned"], replaced
    assert replaced["registers"]["eax"] & 0xFFFF == 0x1234
    # pushing a second return address shifts every argument by one slot
    (pushed,) = call(dosbox, {**body, "return": "push"}).json()["results"]
    assert pushed["registers"]["eax"] & 0xFFFF == 0xAAAA


def test_stack_at_the_top_of_its_segment(dosbox, routines):
    # SP=FFFCh: the far return slot ends at the top of the segment and the
    # return wraps SP to 0000h; that is still the caller's stack level
    seg, _ = routines
    for mode in ("push", "replace"):
        esp = 0x10000 if mode == "push" else 0xFFFC
        (res,) = call(dosbox, {"kind": "far16", "segment": seg, "offset": OFF_BOX_RETF, "return": mode,
                               "registers": {"esp": esp & 0xFFFF, "ds": seg}}).json()["results"]
        assert res["returned"], (mode, res)


def test_dos_calls_outside_the_allow_list_are_unrestored(dosbox, routines):
    seg, _ = routines
    (res,) = call(dosbox, {"kind": "far16", "segment": seg, "offset": OFF_CHDIR,
                           "registers": {"ds": seg}}).json()["results"]
    assert res["returned"], res
    assert {"handler": "DOS Int 21", "ah": 0x3B, "count": 1} in res["handlers"]
    assert any("AH=3Bh" in u for u in res["unrestored"])
    # a get-time call goes through the same handler and is restorable
    (ok,) = call(dosbox, {"kind": "far16", "segment": seg, "offset": OFF_GETTIME}).json()["results"]
    assert {"handler": "DOS Int 21", "ah": 0x2C, "count": 1} in ok["handlers"]
    assert ok["unrestored"] == []


def test_timers_let_code_wait_on_the_timer(dosbox, routines):
    seg, _ = routines
    body = {"kind": "far16", "segment": seg, "offset": OFF_WAIT_TICK, "max_cycles": 50_000_000}
    # without timers machine time stands still: the tick never changes
    (frozen,) = call(dosbox, {**body, "max_cycles": 2_000_000}).json()["results"]
    assert not frozen["returned"] and "did not return" in frozen["error"]
    (ticking,) = call(dosbox, {**body, "timers": True}).json()["results"]
    assert ticking["returned"], ticking
    assert ticking["timer_ticks"] > 0
    assert any("machine time" in u for u in ticking["unrestored"])


def test_wrong_mode_and_bad_requests(dosbox, routines):
    seg, _ = routines
    r = call(dosbox, {"kind": "near32", "segment": seg, "offset": 0})
    assert r.status_code == 400 and "protected mode" in r.json()["error"]
    assert call(dosbox, {"kind": "far16", "segment": seg}).status_code == 400
    assert call(dosbox, {"kind": "far16", "segment": seg, "offset": 0, "bogus": 1}).status_code == 400
    r = call(dosbox, {"kind": "far16", "segment": seg, "offset": 0,
                      "ranges": [{"start": 0xFFFFFF00, "end": 0xFFFFFFFF}]})
    assert r.status_code == 400 and "outside" in r.json()["error"]


def test_not_paused(dosbox, routines):
    seg, _ = routines
    dosbox._post("/api/v1/debug/continue")
    for _ in range(50):
        if not dosbox._get("/api/v1/debug/status").json().get("debugging"):
            break
        time.sleep(0.1)
    r = call(dosbox, {"kind": "far16", "segment": seg, "offset": OFF_BOX_RETF})
    assert r.status_code == 200 and r.json()["status"] == "not_paused"
    dosbox._post("/api/v1/debug/pause")

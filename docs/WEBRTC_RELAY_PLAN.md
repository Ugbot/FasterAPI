# WebRTC + Relay — Punch-Card Plan

> **Goal:** Get a *proper*, working version of WebRTC and the signaling **Relay** feature
> running on the FasterAPI native server, **verified in Chrome via the chrome-devtools MCP**.
>
> This plan was reconstructed from a full source audit on 2026-05-24 (the originally
> referenced `docs/WEBRTC_RELAY_PLAN.md` did not exist in the tree). Status reflects the
> **code**, cross-checked against `PROJECT_MAP.md` / `FASTERAPI_AUDIT.md`.
>
> Work the cards in order. Tick each `[ ]` → `[x]` as it lands. Don't skip; don't mock.

---

## Ground truth (audit findings)

- **The relay = WebRTC signaling.** A WS endpoint relays `offer`/`answer`/`ice-candidate`
  between browser peers by `target` peer id, plus `peer-joined`/`peer-left`/`peers` room events.
- **Python `fasterapi/webrtc/signaling.py` `RTCSignaling` is the functional relay** (real
  `ws.send_text`, broadcast, room mgmt). **C++ `src/cpp/webrtc/signaling.cpp` `send_to_peer`
  is a stub** (`std::cout` only, never sends) — not on the live path.
- **The live demo (`examples/webrtc_demo.py`) cannot run today:** the FasterAPI **native
  C++ library is not built** → `import fasterapi` reports "Using fallback mode", so there is
  no WebSocket server.
- **Why the Windows build fails (root causes):**
  1. `fasterapi_http` (SHARED) has **no exported symbols** on MSVC — no `__declspec(dllexport)`,
     no `WINDOWS_EXPORT_ALL_SYMBOLS`. ctypes loads the DLL but can't find `http_server_create`.
  2. `external/coroio` (empty submodule) and `external/simdjson` (absent) break the **Cython**
     extensions — but the demo path uses the **ctypes** path (`fasterapi/http/server.py` →
     `bindings.py`), which needs only `libfasterapi_http.dll`. So we **skip Cython** for now.
- **Toolchain present:** CMake 4.3, Ninja 1.10, Python 3.12, uv, VS 2022 Community (MSVC).
- DLL naming already matches: CMake forces `lib` prefix → `libfasterapi_http.dll`; `bindings.py`
  searches `fasterapi/_native/libfasterapi_http.dll`. ✅
- Headless Chrome has no camera, so the **video** demo can't be auto-verified. The relay will be
  verified with a **data-channel** loopback (two `RTCPeerConnection`s in one page handshaking
  through the relay) — this exercises the full offer/answer/ICE relay path without `getUserMedia`.

---

## Punch cards

### PC0 — Audit & write this plan ✅
- [x] Ground-truth WebRTC/relay code, build system, toolchain, Windows blockers.
- [x] Write this punch-card plan.

### PC1 — Build the native `fasterapi_http` DLL on Windows (MSVC)
- [ ] Add `WINDOWS_EXPORT_ALL_SYMBOLS ON` to the `fasterapi_http` target (export the C API).
- [ ] Configure CMake with MSVC: HTTP on, MCP off, PG off (not needed), benchmarks off.
- [ ] Build the `fasterapi_http` target; fix MSVC compile/link errors as they surface.
- [ ] Confirm `fasterapi/_native/libfasterapi_http.dll` exists and **exports `http_server_create`**.

### PC2 — Make the ctypes path load the native server
- [ ] `import fasterapi` no longer prints "fallback mode"; `fasterapi.http` loads the DLL.
- [ ] Smoke test: start an `App`, hit a GET route over HTTP, get a real response.
- [ ] Smoke test: a WebSocket echo endpoint accepts a browser/CLI WS client and echoes.

### PC3 — Proper relay signaling server
- [ ] Replace hardcoded `room_id="default"` with the real `?room=` query param.
- [ ] Ensure relay shared state is correct for the run model (single process for the demo;
      document the multiprocess caveat for `RTCSignaling` in-memory maps).
- [ ] Write `examples/webrtc_relay.py`: WS `/rtc/signal`, GET `/rtc/stats`, GET `/rtc/relay-test`
      (data-channel loopback page) and GET `/rtc/demo` (video page, manual).

### PC4 — Chrome-verifiable data-channel relay test page
- [ ] `/rtc/relay-test` opens two WS peers in one room, runs full offer/answer/ICE through the
      relay, opens an `RTCDataChannel`, round-trips a random message peer-to-peer.
- [ ] Page exposes a machine-readable result (e.g. `window.__RELAY_RESULT__` / a DOM `#result`
      element with `RELAY_OK:<echo>` or `RELAY_FAIL:<reason>`).

### PC5 — Verify with Chrome via chrome-devtools MCP
- [ ] Launch the relay server.
- [ ] Drive Chrome MCP to load `/rtc/relay-test`; wait for `RELAY_OK`.
- [ ] Confirm via console logs + screenshot that the data channel opened and the message
      round-tripped through the relay. Capture `/rtc/stats` showing offers/answers/ICE relayed > 0.

### PC6 — Tidy & document
- [ ] Update `PROJECT_MAP.md` WebRTC/relay + Windows-build rows to reflect reality.
- [ ] Note any remaining gaps (C++ `send_to_peer` stub, ICE/STUN TODOs, multiprocess relay).

---

## Verification log
*(filled in as cards complete)*

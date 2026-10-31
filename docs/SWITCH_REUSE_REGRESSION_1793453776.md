# Capture 1793453776: continuous-mode regression

Hardware build: CI #74, head 5c0786a9, merge dbad424abca98c6f5d6131467857c322107cd863.

- Session: 245.843302 seconds; 5,453 contiguous render IDs; all reported overflow counters zero.
- Controls: 76 PASS, 7 SKIP, 0 FAIL. This is not a performance pass.
- Continuous controller: started=true, stopped=true, failed=false; 218 active renderer callbacks, blockers=4; source-policy global_denials=0 and backend_supported=true.
- Resource interface: 294 evaluations, 286 reference evaluations, 8 actual no-Hash hits (44,032 logical bytes), 2 candidate audits, zero recorded mismatch. No candidate proof for the subsequent full-Hash period.
- In the tested controller, the above stop state maps to the 100,000 watched-store ceiling. The legacy log lacks the stop tick and actual store/page-only counter; do not invent a precise stop time or blame a particular texture/page.
- HLE range ledger: 22,807,084 calls/exits, identical to the session HLE-import call count. It continued until exit. 8,594,819 range scans and 1,130,228 independent outputs were recorded. These are operation counts, not measured mutex CPU time.
- Source inspection confirms the terminal branch only reopened the write PTEs. It did not disable epoch::enabled or the runtime callbacks/source-policy ingress. Thus full Hash resumed while shared tracker overhead remained.
- Whole-session 95th/99th render interval is approximately 75.61/255.49 ms, maximum 642.50 ms. Startup/loading/navigation are included. The 120–230 s window has 2,640 recorded render callbacks and about 24.0 Loop/s; it is uncontrolled normal play, not a paired benchmark or a claim of device FPS.

Resolution: withdraw implicit continuous mode, retire new domain/callback registrations on terminal fallback while preserving outstanding leases and old JIT write-table lifetime, and reject excluded sources before page registration. Preserve the tested limited reuse algorithm only behind explicit opt-in. Keep raw counters and prior fixed-scene result; do not claim it measures total instrumentation overhead or universal performance.

No repeat of this capture or another static experiment is requested. New hotfix device net benefit is not yet measured.

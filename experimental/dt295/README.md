# Experimental PIX-DT295 primitives

This is an incomplete research prototype for USB `06b8:103b`, separate from the
stable PIX-DT181 command. It cannot yet capture live TS and is not a release.

Implemented primitives: libusb control/bulk/interrupt transport, idle firmware
upload, initial firmware authentication, 30-second authentication renewal,
terrestrial tuner setup, TRC firmware upload, TS-key requests, target-0 receive
preparation/start/stop, and offline TS payload decryption.
Only target 0 is addressed. Firmware and authentication material are read from
the owner's separately supplied official package; they are not embedded here.

Build and run synthetic tests with `make test` (Python 3 and a C compiler).
Actual USB access also requires libusb 1.0. The library builds on macOS and Linux.

Current diagnostic commands:

- `python3 dt295.py status`: inspect firmware state.
- `python3 dt295.py boot /path/to/idle.bin`: upload only from cold state 0.
- `python3 dt295.py decode key.bin iv.bin input.ts > output.ts`: offline decode
  with explicit 16-byte TS key/IV files. Control-session keys are different.

`receiver.py` accepts a separately supplied private `ReceiveProfile` containing
three key-request words and validated TRC register fields. It requests the key
before uploading/preparing TRC, checks the exact 16-byte reply, and allows only
one start attempt per prepared object. On stop it checks the actual hardware
state, including after a start timeout. A failed attempt must not be retried
without diagnosing and restoring the device state.

There is no complete capture command, device listing, mirakc integration, or
stable CLI compatibility in this prototype.
Callers must serialize device access. `renew_if_due()` services authentication
only after its 30-second deadline; an error must abort the session. Do not
re-send a timed-out firmware upload or start command. The boot path retries
only status reads across a firmware restart.

Mac hardware observations (2026-10-12): initial authentication and three
successive renewal exchanges succeeded; T27 lock-condition registers were
observed in a separate tuning probe. Applying the driver's default TRC fields
reaches prepared state 1, but starting TS still leads to state `0xffff` and
firmware error `0x03030006`, with no bulk TS received. This also happens with a
recent successful authentication renewal. Its cause remains unknown. Do not
reuse the failing start sequence as a working receiver.

Offline decryption matched an independent AES implementation for 32 captured
Windows packets. This does not validate live capture or session-key acquisition.
Linux testing covers compilation and synthetic regression tests only; Linux
hardware reception has not been tested. Keep the official Windows environment
for reference until live receive/decrypt/stop/restart are independently verified.

The successful Windows reload trace supplied a revised profile (including PID
filters) for the next Mac test. Its first probe completed initialization,
authentication and tuning, then stopped before TRC upload because an experimental
check treated the encrypted prefix at response offset +12 as a status. Driver
41b94 checks status at +4 and copies data from +16; the response status was zero
with four payload words. That extra check has been removed and a synthetic
regression accepts a nonzero prefix while rejecting actual error status. Live TS
with the revised profile remains unverified.

A follow-up acquired the 16-byte TS key and prepared TRC. The prototype initially
misread a queued state 1 as start failure: acknowledging state-event bit 4
revealed running state 2. State-event draining and bounded waits are now
implemented. Stop follows 2 -> pause state 3/4 (DMA input dependent) -> 1 -> idle,
instead of issuing idle directly while running. The device later reached error
`0x11000001` during diagnosis; no bulk TS was collected and the corrected full
lifecycle still requires a fresh hardware test. Synthetic tests cover stale
state events, firmware error, timeout, pause selection, and no repeated start.

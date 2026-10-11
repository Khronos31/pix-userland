# Experimental PIX-DT295 primitives

This is an incomplete research prototype for USB `06b8:103b`, separate from the
stable PIX-DT181 command. It has received live TS in a bounded Mac probe, but lacks video/audio
packets and is not a usable tuner release.

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

Latest Mac probe: EP81-only reception completed 40 seconds (67,483,136 bytes,
358,952 packets), one authentication renewal and normal return to idle. The
pause path now keeps draining USB input; 65,536 queued bytes had to be consumed
before firmware acknowledged pause. Independently decoded 1,024 encrypted sample
packets matched CommonCrypto, and reassembled PAT/PMT sections passed CRC checks.
However, PMT-declared video/audio PIDs 256/272/273 were absent; valid subtitle and
other sections do not establish working video/audio. Two probes polling inactive
EP84/86 also suffered spontaneous device resets; EP81-only success is evidence
for further isolation, not proof of the reset cause. Windows remains needed.

Earlier probes exposed three corrected lifecycle errors: response offset +12 was
incorrectly treated as status (the actual status is +4), a queued state 1 was
misread as failed start before acknowledging state-event bit 4, and stopping
required draining pending output before transition from running state 2 to pause
3/4, then 1 and idle. Synthetic tests cover these cases and abort on disconnect
or a bounded pause timeout without forcing another state-changing command.

The current stop drain reads only target-0 EP81. This narrow change is covered
by synthetic tests; the completed hardware probe still used the earlier stop
loop that also polled EP84/86. No repeat hardware validation is claimed for the
narrowed stop loop.

Linux testing covers compilation and synthetic regression tests only; Linux
hardware reception has not been tested. Keep the official Windows environment
for reference until live video/audio receive/decrypt/stop/restart are verified.

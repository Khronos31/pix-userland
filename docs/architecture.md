# Architecture

`pix-ts` is a single-receiver userspace CLI. Runtime dependencies are libusb and
standard OS interfaces (POSIX threads on Unix, Win32 synchronization on Windows). There is no libudev, OpenSSL,
PC/SC, Python, systemd, or daemon API in the application.

- `protocol.c`: DT181 power sequencing, FPGA upload, I2C tuner setup, terrestrial
  and satellite PLL/TSID/LNA, device session and keepalive. USB operations go
  through `pixy_io.bulk`; no libusb types enter this module.
- `crypto.c`: AES-128 ECB/CTR and 1024-bit RSA for the legacy device handshake.
  Fresh random RSA primes are generated per session. No private RSA key is
  distributed in the executable. Trial division and 64 randomized Miller–Rabin
  rounds precede modulus construction. Tests include FIPS AES and an independent
  Python big-integer cross-check during development. This is a device protocol
  implementation, not a general cryptography API; the legacy 1024-bit RSA
  requirement is fixed by the device. RSA exponentiation is not constant-time.
- `transport.c`: key-table fragments, transfer AES context, 188-byte framing,
  streaming CTR across TS packets; PAT/PMT CRC and continuity checks. The current
  and immediately preceding key tables coexist during device key rotation; new
  tables can arrive while stream markers still reference the previous generation.
  Unknown IDs in both generations remain fatal. USB transfer
  cryptography is distinct from broadcast B-CAS/MULTI2. No software B25/MULTI2
  decoder or card-reader API is implemented.
- `usb_libusb.c`: device enumeration/open or Android borrowed-fd wrapping, 16
  asynchronous 32 KiB bulk reads, cancellation and interface lifecycle.
- `main.c`: CLI, bounded 32 MiB raw-input queue and output worker. Slow outputs do
  not block libusb's event handler. A full queue fails the capture rather than
  reporting an intact TS. Nonblocking pipe output times out after 3 seconds
  without progress. All logs and final JSON go to stderr.
- `platform.c`: monotonic time/sleep, output mode, and OS entropy. Unix uses `/dev/urandom`; Windows uses BCryptGenRandom. `os.h` abstracts threads, mutexes and condition variables.

Initialization: load private assets → claim USB → power/FPGA → session → tuner
setup → select channel → clear FIFO → queue USB reads → start stream → worker.
The FIFO clear must precede queued reads: queuing reads before clearing produced
one non-TS byte in the first C hardware test. The corrected order matches the
Python reference and passed all three bands without resynchronization.

Shutdown: command stream stop while reads are active → cancel and reap all
transfers → drain bounded queue/join worker → power off → release USB. Errors and
SIGINT/SIGTERM take the same cleanup path once the device is open. The USB fd
provided by Android remains owned by the caller.

Future models must get their own verified USB IDs, firmware, power and tuner
profiles. DT141/195/295 are not claimed to work. The current backend only opens
06b8:102b; the name `pix-userland` is not a blanket compatibility claim.

Windows uses libusb's WinUSB backend and native Win32 synchronization. Its CI build and unit tests do not certify USB reception. WebUSB remains unimplemented and needs an asynchronous browser adapter.

## Bounded diagnostic capture (0.1.3-dev2)

`--diagnostic NEW_DIRECTORY` records USB input locally while ordinary TS output can go to `/dev/null`.
The directory must not exist. On POSIX, directories use mode 0700 and files use mode 0600.
Checkpoints contain session keys, decoder state and broadcast data: never commit or upload them.

Two independent capture lanes prevent a CC change from consuming the stream-error evidence:

- The top-level ring has eight 8 MiB segments. The first resynchronization, IV wait or decoder failure freezes it and saves an 8 MiB post window.
- `cc-01` through `cc-03` each have two 8 MiB ring segments and a 2 MiB post window. A CC, TEI, PSI CRC, invalid-packet or scrambled-packet increase freezes the current window; after its post window completes, the next window starts with the existing cumulative CC counter. Further validation errors during a post window are captured there. After three windows, CC events remain logged while the independent stream ring stays active.

The total bound, including checkpoints, per-frame overhead, one-frame overshoot and event logs, remains below 200 MiB. Frame headers count toward segment/post limits, including tiny USB completions. Root events are capped at 8 MiB, each CC-window log at 1 MiB. The pre-event time range is shorter than in the original diagnostic build; it depends on bitrate and the current rotating slot position. No complete long-duration TS is retained.
A fatal receiver/USB error may terminate before a post window finishes. On normal shutdown, unfinished windows remain readable once files close.

Every segment starts with a decoder and checker checkpoint. Raw frame headers record receive/process timestamps, byte counts and queue depth. `events.log` records PID/CC, cumulative errors and periodic packet/queue/delay samples. USB completion statistics remain on stderr. Root logs also describe current, CRC-valid PMT section changes without changing continuity error counting:

- `pmt_begin`: `pid` = PMT PID, `expected` = programme number, `actual` = version.
- `pmt_es`: `pid` = elementary-stream PID, `expected` = stream type, `actual` = PMT PID.
- `pmt_end`: `pid` = PMT PID, `expected` = section number, `actual` = last section number.

Each begin/end group describes a section's complete ES list, allowing additions/removals to be compared offline. Multiple sections/programmes must be considered together. CRC-invalid, future and structurally malformed PMTs do not update this observation state. CC history is deliberately not cleared when a PMT changes; a new map alone is not sufficient evidence to suppress a discontinuity.

Diagnostic writes run in the decoder worker and may affect scheduling. Queue delay exposes this overhead. I/O failures make the run fail explicitly. Normal capture without diagnostics writes no diagnostic files.

Replay trusted, completed files with the same build and ABI:

```sh
pix-ts --replay-diagnostic /private/path/ring-03.pixdiag > /dev/null
pix-ts --replay-diagnostic /private/path/ring-03.pixdiag --replay-chunk-bytes 187 > /dev/null
```

The optional split size (1..32768) subdivides each recorded frame before feeding the decoder, preserving byte order. Compare output hashes and counter deltas across splits; this tests dependence on feed boundaries, not reception loss before USB delivery. Default replay preserves recorded frame boundaries.
The native-state checkpoint format is private, unsuitable for untrusted or cross-version input. The checker layout changed in dev2, so original dev1 snapshots must use the retained original executable or a separately verified ABI-compatible analysis harness, not the dev2 executable.
Generation numbers give ring order; slot names wrap. Final replay counters include checkpoint history: use differences from starting counters. Replayed TS is only that segment's output, not a complete programme recording. Keys/context are never printed in diagnostic logs.

### USB diagnostic logging (0.1.3-dev3, pending hardware validation)

Periodic USB telemetry is queued to a separate writer thread. The completion callback never writes normal diagnostic records to stderr: a slow log file must not delay resubmitting bulk transfers. The queue holds 128 metadata-only records; when full, it drops telemetry and counts `log_dropped`, rather than blocking USB reception. The logger releases its queue lock before calling the writer. Writer failures fail the diagnostic run, and shutdown drains the logger after USB transfers have stopped. Final `log_dropped_total` records any telemetry loss. Callback gaps of at least 100 ms are logged alongside periodic records, subject to the existing abnormal-event log limit. Fatal USB error messages can still be printed on the error path.

A synthetic blocked-writer test holds the writer indefinitely while the producer submits more than the queue capacity. Submissions must finish before the writer is released; ordering, bounded drops and write-error propagation are checked. This fixes the synchronous diagnostic logging path, not yet every possible source of receiver starvation. The original non-diagnostic Release did not use this periodic logging path. Existing dev2 hardware trials continue on their original executable; dev3 has not yet been validated by a new hardware soak.

The POSIX sink uses low-level writes and checks shutdown while waiting for a full single-writer pipe, without changing shared descriptor flags. Shutdown may discard telemetry; this makes the diagnostic run fail. OS-blocked regular-file writes, competing pipe writers, and arbitrary non-cooperative custom writers have no portable hard shutdown deadline. `log_previous_write_ms` and `log_max_write_ms` measure completed writer calls; a call appears in the next metadata record. Final JSON `capture_elapsed_ms` measures the live receive loop only, excluding initialization and cleanup (zero for replay/offline).

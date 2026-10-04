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

## Bounded diagnostic capture (0.1.3-dev)

`--diagnostic NEW_DIRECTORY` records the first incident locally while the usual TS output can be redirected to `/dev/null`.
The directory must not exist. On POSIX it is created with mode 0700, and diagnostic files use mode 0600.
These files contain private session keys, decoder state and broadcast data; do not commit or upload them.

The worker records the original USB completion chunks in sixteen rotating 8 MiB segments, preserving roughly one minute at the observed terrestrial bitrate.
Each segment starts with a decoder and TS-checker checkpoint, so replay does not depend on receiving a new key table within the captured window.
At the first resynchronization, IV wait, TS validation error or decoder failure, the ring is frozen and up to 16 MiB of subsequent input is saved separately.
The bound, including checkpoint overhead, frame headers and the 8 MiB event-log limit, is below 200 MiB. Only the first incident gets raw data; later counter changes remain in the bounded event log.
A fatal receiver/USB error may end the run before the post-event window is complete; the last ring remains available.

`events.log` records monotonic and Unix timestamps, PID/expected/actual continuity counters, cumulative decoder anomalies, queue high-water and processing delay.
Frame headers retain each callback's length and receive/process timestamps. USB status/short-transfer and periodic completion counters are written to stderr.
The diagnostic file writes occur in the decoder worker and can affect scheduling; queue delay is recorded to expose that effect. They are not made from the USB callback.
Disk I/O failure makes the diagnostic trial fail explicitly. Normal capture without this option does not write diagnostic files.

Replay a trusted, completed segment using the same binary/build and ABI:

```sh
pix-ts --replay-diagnostic /private/path/ring-03.pixdiag > /dev/null
```

The checkpoint format is a private native-state snapshot, not an interchange format. It is unsuitable for untrusted input or arbitrary cross-version/architecture replay.
Generation numbers, printed on replay and in `events.log`, give ring order; slot filenames wrap.
Reported counters include the state at the segment's start; compare with the printed checkpoint counters to distinguish new errors from inherited ones.
Raw-frame boundaries are replayed unchanged, including any partial TS packet in the checkpoint. Replayed TS is only that segment's output, not a complete programme recording.
The context/key bytes are stored only in the private checkpoint and never printed in diagnostic logs.

# Architecture

`pix-ts` is a single-receiver userspace CLI. Runtime dependencies are libusb and
standard OS C/POSIX interfaces (including pthreads). There is no libudev, OpenSSL,
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
  streaming CTR across TS packets; PAT/PMT CRC and continuity checks. USB transfer
  cryptography is distinct from broadcast B-CAS/MULTI2. No software B25/MULTI2
  decoder or card-reader API is implemented.
- `usb_libusb.c`: device enumeration/open or Android borrowed-fd wrapping, 16
  asynchronous 32 KiB bulk reads, cancellation and interface lifecycle.
- `main.c`: CLI, bounded 4 MiB raw-input queue and output worker. Slow outputs do
  not block libusb's event handler. A full queue fails the capture rather than
  reporting an intact TS. Nonblocking pipe output times out after 3 seconds
  without progress. All logs and final JSON go to stderr.
- `platform.c`: monotonic time/sleep. OS entropy access is currently in `crypto.c`
  (`/dev/urandom`); a Windows/browser port must provide an equivalent CSPRNG.

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

WinUSB may eventually use libusb's Windows backend. WebUSB needs a browser
adapter and an asynchronous command/state-machine bridge, not merely a different
linker flag. Neither Windows nor browser support is implemented in this version.

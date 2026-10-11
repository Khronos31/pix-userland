# Experimental PIX-DT295 primitives

This is a bounded research prototype for USB `06b8:103b`, separate from the
stable PIX-DT181 command. Mac hardware has received and decrypted terrestrial
video/audio and fed it directly to FFmpeg. It is not a stable tuner release.

Implemented primitives: libusb transport, idle firmware upload, initial firmware
authentication and 30-second renewal, card interface initialization/activation,
terrestrial tuning, TRC firmware upload, TS-key requests, target-0 receive
preparation/start/stop, TS payload decryption, and a cold-start capture command.
Only target 0 / EP81 is addressed. Firmware, authentication material, the receive
profile and IV are separately supplied private files, never embedded in source.
Host-side card APDU handling is not implemented.

Build and run synthetic tests with `make test` (Python 3 and a C compiler).
Actual USB access also requires libusb 1.0. The library builds on macOS and Linux.

From this directory, after reconnecting the USB device with antenna/card fitted:

```sh
python3 capture.py --assets /path/to/official/Driver/selene/usb/x64 \
  --profile /path/to/private/successful-profile.json \
  --iv /path/to/private/aes-iv.bin --seconds 40 T27 \
  | ffmpeg -v error -f mpegts -i pipe:0 -t 10 \
      -map 0:v:0 -map 0:a:0 -f null -
```

The capture command writes decrypted TS to stdout and diagnostics to stderr.
The decoder discards its output. Closing the consumer triggers receiver cleanup.
Capture duration must be greater than zero and at most 60 seconds. This command
requires cold state 0; it does not resume an expired authentication session.
There is no device listing, mirakc integration, or stable CLI compatibility.
The private receive profile still derives from the owner's Windows reference;
it is not a general-purpose configuration generator.

Other diagnostic commands:

- `python3 dt295.py status`: inspect firmware state.
- `python3 dt295.py boot /path/to/idle.bin`: upload only from cold state 0.
- `python3 dt295.py decode key.bin iv.bin input.ts > output.ts`: offline decode
  with explicit 16-byte TS key/IV files. Control-session keys are different.

`receiver.py` validates a private `ReceiveProfile`, requests the TS key before
TRC preparation, checks its exact 16-byte reply, and permits one start attempt
per prepared object. Callers must serialize device access. `renew_if_due()`
services authentication after its 30-second deadline; errors abort the session.
Never resend a timed-out firmware upload or start command. Stop drains target-0
EP81 while awaiting pause, then returns to idle. Disconnects and bounded pause
timeouts abort without forced state-changing retries.

The missing video/audio investigation identified a missing card interface setup:
command 0x0a/subcommand 0/value 1 precedes activation subcommand 1/value 1.
Both use the secondary B9 command lane. Card notification state is read before
acknowledgment; pending APDU responses are not discarded. Activation reached
state 3 after interface initialization. Adding unrelated SDK command 07030400
was rejected: the successful Windows trace also returned firmware status
0x00020018 for those calls. SDK return zero alone is not firmware success.

Mac validation on 2026-10-12:

- A 40-second probe received 146,978,816 bytes / 781,802 packets, including
  341,501 video PID 256 packets and 7,649 audio PID 272 packets. Its PMT advertised
  audio PID 272, not 273. Authentication renewal and EP81-only stop succeeded;
  65,536 bytes were drained and the final state was idle 0x11.
- Independently decrypting 1,024 encrypted samples with CommonCrypto matched all
  packets; reassembled PAT/PMT CRCs passed. FFprobe recognized MPEG-2 video
  1440x1080 and AAC audio 48 kHz stereo. The bounded sample ends mid-frame and
  therefore does not establish continuous decoding quality.
- A separate cold-start capture piped directly into FFmpeg decoded 280 video
  frames with the audio stream selected, reaching 10.010 seconds. Both processes
  exited zero. Initial `Invalid frame dimensions 0x0` messages were present;
  this was not an error-free decoder run. TS and decoded media were discarded.
- A later read after the capture process had exited found state 0xffff and error
  0x11000001. Authentication expiry after exit is a hypothesis, not a verified
  cause. Repeat capture without reconnecting and clean session shutdown remain
  unresolved. The capture cleanup now logs its immediate final state to stderr;
  this added log has synthetic coverage but has not been rerun on hardware.

Earlier inactive EP84/86 polling probes suffered spontaneous device resets;
EP81-only success does not prove their cause. Full-period TS was not retained,
so independent validation is limited to saved samples and the short decoder run.
Mac and Linux Alpine 3.22 x86_64 musl synthetic tests pass. Linux hardware, BS,
long-running reception and start-after-stop reuse remain unverified. Keep the
Windows application/driver for reference while these lifecycle gaps are resolved.

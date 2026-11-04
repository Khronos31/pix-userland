# PIX Core integration build

This CMake project builds the released `pixd`, `pixctl` and `pix-ts` programs: the
existing PIX DT181 and DT295 C backends with the shared Core receiver, daemon,
control and TS client interfaces. The normal `make` and `make test` build only
the legacy standalone CLI (`pix-ts-legacy`), which is not distributed.

The Core revision is pinned in `dtv-userland-core.revision`. The build uses the
source archive's `third_party/dtv-userland-core` when present and otherwise fetches
the pinned revision. A local source tree can be supplied for development:

```sh
cmake -S core-integration -B build/core-integration \
  -DPIX_CORE_SOURCE_DIR=/path/to/dtv-userland-core
cmake --build build/core-integration --target pix_dt181_receiver_adapter_proof
ctest --test-dir build/core-integration --output-on-failure
```

The adapter proofs use synthetic USB and tuner behavior. They do not claim a
physical receive test or provide a daemon/CLI entry point.

The DT295 adapter provides terrestrial tuning and satellite IF tuning with an
explicit TSID or a slot from the live demod TSID table. Satellite support reuses
the DT295 satellite register sequence from the existing research. The offline
lifecycle proof covers satellite tune/TSID/slot, CAT-less satellite tables,
terrestrial CAT retention, and delayed TSID reflection; physical satellite
reception and long-running behavior remain separate verification.

Core's owner lifecycle releases/detaches the receiver before backend shutdown.
The adapter proof covers that order; direct shutdown while still attached is
not part of this increment.

## Optional L3 adapter skeleton

`PIX_BUILD_L3_ADAPTER=ON` builds the optional PIX L3 product adapter. It is
off by default, so the released `pixd`, `pixctl`, `pix-ts` and the custom-FW
workflow are unchanged. The adapter consumes the Core prototype header at
`PIX_L3_CORE_PROTOTYPE_DIR` (default:
`${PIX_CORE_SOURCE_DIR}/prototype/l3_owner`). The current candidate is Core
branch `wip-l3-product-integration-20261104`, API
`253053da51184af03abebb15e7c129c465d90dee` plus G1
`c5a54eec5c335b09b550db1dcb44a0bdf539236d`, initialization transfer
`01c995170f9459da44b0fda176eb79e2a12eecbd`, queue
`bdc5a03c3087b0906a198cf793fae77195ea19c3` and G8
`2681542fe7abd51520121df8afff9fe1546cd76f`, build/CI
`347f1ff54d1e6381a7690f404a9c0e5e708b7752`.

```sh
cmake -S core-integration -B build/pix-l3 \
  -DPIX_CORE_SOURCE_DIR=/path/to/dtv-userland-core \
  -DPIX_L3_CORE_PROTOTYPE_DIR=/path/to/dtv-userland-core/prototype/l3_owner \
  -DPIX_BUILD_L3_ADAPTER=ON
cmake --build build/pix-l3 --target pix_l3_adapter_contract
ctest --test-dir build/pix-l3 -R pix_l3_ --output-on-failure
```

The host side is the Core `dtv_l3_usb_provider`; PIX supplies
`dtv_l3_product_adapter` and `dtv_l3_adapter_factory`. `pix_l3_factory_create`
takes the USB-only backend seam and product semantics, then
`pix_l3_factory_usb_provider` / `pix_l3_factory_adapter_factory` produce the
two Core inputs. `dtv_l3_open_factory` receives model, selector and the
firmware source (`PATH`, `BYTES` or `READ_CALLBACK`); the adapter copies the
image because the Core view is valid only during create, passes it to the
product configure callback, and keeps it alive through the owner lifetime.

The adapter keeps PIX product meaning out of Core: DT181 starts the satellite
stream before tune while DT295 tunes first, both expose presence observation
without external APDU, neither has an LNB power scope, and route/plan, firmware
bootstrap, decoded TS output and custom firmware remain product-side. The
existing DT181 `usb_backend` bulk path and DT295 `dt295_io` synchronous seam are
the backend implementations. Their underlying stop is pipe-wide, so both report
`DTV_L3_CANCEL_SCOPE_PIPE`; neither currently has a per-transfer cancel handle,
so `cancel_supported` is 0 and a cancel request is reported as `UNSUPPORTED`
rather than treated as success.

The USB seam also accepts an optional asynchronous `submit`/`cancel_async` pair.
When present, submit must copy OUT data and retained state before returning
`ACCEPTED` and cancel must return without waiting for a completion callback.
PIX keeps `call_flags` at zero by default: an async shape alone is not a
promise that a Linux provider cannot wait on a lock inside a callback frame.
The common owner keeps the claim, buffers and terminal completion when a cancel
races a normal completion.

The real PIX backends now use their existing pipe-wide cancel:
- DT181 starts the existing `usb_stream_start` path and `cancel_async` calls
  `usb_stream_stop`, which cancels the existing libusb stream transfers and
  drains their terminal callbacks.
- DT295 uses an existing `dt295_stream` handle: a reader thread waits on
  `dt295_stream_read`, and cancel calls `dt295_stream_stop` (PIPE).
Both report `cancel_scope=PIPE`, not transfer-level cancel. A per-transfer
cancel still needs a product-side transfer handle that the current backends do
not expose. The fixtures use a
fake asynchronous pipe provider to verify CANCELLED/terminal retention, a raced
normal completion, ownership retention until both completions, and an
`UNSUPPORTED` cancel that does not become success. Firmware is required for both
models.

The fixtures cover factory/capability differences, all three firmware sources,
G1 bind/initialize, a real owner transfer from initialize
(`transfer_start` -> provider submit -> completion -> `op_wait`/`op_release` ->
READY), failed-initialize open failure, and the capability catalog. The real
DT181/DT295 cancel fixtures drive the existing stream handles through the owner
provider, assert PIPE scope, terminal CANCELLED completion, release BUSY while
an op is outstanding, and release/close after completion. They also
cover one stream IN path through the same owner/provider route: `stream_attach`
starts a bulk IN via `dtv_l3_transfer_start`, delivers the completed TS to the
Core stream, and the test reads it back before stop/release; a second case lets
close retire an active stream and verifies lease/claim recovery. Fake async
completion and the DT295 `dt295_io` backend seam are exercised separately.
Route rejection, transfer input copy and repeat polling, DT295 `dt295_io`
control/bulk/deadline mapping, per-transfer/pipe cancel scope and terminal
completion retention, and close retention with explicit cleanup retry remain
covered. Physical USB, real receivers, CARD/TS concurrency and
custom firmware are not verified by this target.

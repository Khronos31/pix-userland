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
`${PIX_CORE_SOURCE_DIR}/prototype/l3_owner`); at the time of writing the fixed
candidate is Core `8bf496c9f74cff1d54826032635d074f64700f4c`.

```sh
cmake -S core-integration -B build/pix-l3 \
  -DPIX_CORE_SOURCE_DIR=/path/to/dtv-userland-core \
  -DPIX_L3_CORE_PROTOTYPE_DIR=/path/to/dtv-userland-core/prototype/l3_owner \
  -DPIX_BUILD_L3_ADAPTER=ON
cmake --build build/pix-l3 --target pix_l3_adapter_contract
ctest --test-dir build/pix-l3 -R pix_l3_ --output-on-failure
```

The adapter keeps PIX product meaning out of Core: DT181 starts the satellite
stream before tune while DT295 tunes first, both expose presence observation
without external APDU, neither has an LNB power scope, and route/plan, firmware
bootstrap, decoded TS output and custom firmware remain product-side. The
existing DT181 `usb_backend` bulk path and DT295 `dt295_io` synchronous seam are
the backend implementations; control transfers and per-transfer cancel that the
current PIX backends do not expose are reported as `UNSUPPORTED` rather than
created as a second path.

The fixtures cover factory/capability differences, route rejection keeping a
free lease, transfer input copy and repeat polling, DT295 `dt295_io` control,
bulk and deadline mapping, and close retention with explicit cleanup retry.
Physical USB, real receivers, CARD/TS concurrency and custom firmware are not
verified by this target.

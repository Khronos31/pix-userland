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

# PIX Core integration build

This opt-in build target compiles the existing PIX DT181 and DT295 C backends
with the shared Core receiver and packet-stream interfaces. The normal `make`
and `make test` CLI builds remain independent.

The build fetches the pinned Core source revision by default. A local source
tree can be supplied for development:

```sh
cmake -S core-integration -B build/core-integration \
  -DPIX_CORE_SOURCE_DIR=/path/to/dtv-userland-core
cmake --build build/core-integration --target pix_dt181_receiver_adapter_proof
ctest --test-dir build/core-integration --output-on-failure
```

The adapter proofs use synthetic USB and tuner behavior. They do not claim a
physical receive test or provide a daemon/CLI entry point.

The DT295 adapter currently provides terrestrial tuning only. The accepted
PIX product capability includes ISDB-T and ISDB-S; satellite adapter support
remains a follow-up and is not redefined by this terrestrial increment.

Core's owner lifecycle releases/detaches the receiver before backend shutdown.
The adapter proof covers that order; direct shutdown while still attached is
not part of this increment.

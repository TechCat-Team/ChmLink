# ChmLink

ChmLink is a Windows virtual network adapter project for ChmLAN. It aims to
provide Ethernet and IP adapters through a single driver package, with tunnel
protocols implemented in the user-mode agent.

The project is at an early development stage. The packet and batch validation
libraries are portable; the Windows driver, device interface, and installation
tools have not yet been implemented. It cannot replace TAP or Wintun yet.

The compatibility target is Windows 10 version 1607 (build 14393) /
Windows Server 2016 and later. These are development targets, not verified
support. The planned NDIS baseline is 6.60.

## Layout

- `include/chmlink/` — shared C interfaces.
- `src/packet/` — packet validation, independent of Windows APIs.
- `src/protocol/` — experimental batch framing and validation.
- `tests/` — portable C tests.
- `rust/chmlink/` — Rust types for the user-mode integration.

## Test

Run `cmake -S . -B build`, `cmake --build build`, and
`ctest --test-dir build --output-on-failure`. Run `cargo test --workspace` for
the Rust crate.

The protocol uses explicit little-endian fields and bounded batches. The
receiver validates the complete batch before submitting packets. Driver code
must first copy user input into an owned snapshot; parsing a live shared buffer
does not provide protection against concurrent modification. The current
packet checks cover minimum header sizes and IP version, not full protocol
validity. The transport format is experimental and may change before release.

For fuzz testing, build `tests/fuzz_batch.c`, `src/protocol/batch.c`, and
`src/packet/packet.c` with Clang using `-Iinclude` and
`-fsanitize=fuzzer,address,undefined`.

## License

MIT. See [LICENSE](LICENSE).

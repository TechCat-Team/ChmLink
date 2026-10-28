# ChmLink

ChmLink is a Windows virtual network adapter project for ChmLAN. It aims to
provide Ethernet and IP adapters through a single driver package, with tunnel
protocols implemented in the user-mode agent.

The project is at an early development stage. The packet validation library is
portable and tested; the Windows driver, device interface, and installation
tools have not yet been implemented. It cannot replace TAP or Wintun yet.

## Layout

- `include/chmlink/` — shared C interfaces.
- `src/packet/` — packet validation, independent of Windows APIs.
- `tests/` — portable C tests.
- `rust/chmlink/` — Rust types for the user-mode integration.

## Test

Run `cmake -S . -B build`, `cmake --build build`, and
`ctest --test-dir build --output-on-failure`. Run `cargo test --workspace` for
the Rust crate.

## License

MIT. See [LICENSE](LICENSE).

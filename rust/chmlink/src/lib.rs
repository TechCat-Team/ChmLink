//! User-mode types for the ChmLink virtual network adapter.
//!
//! Device creation and packet I/O are not implemented yet.

/// The kind of packet exchanged with an adapter instance.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum Mode {
    /// Complete Ethernet frames, excluding the frame check sequence.
    Ethernet = 1,
    /// IPv4 or IPv6 packets without a link-layer header.
    Ip = 2,
}

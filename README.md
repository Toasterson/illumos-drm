# illumos-drm

A Direct Rendering Manager (DRM) core for illumos written in Rust, and the
drivers built on it.

The project is hosted at <https://code.aopc.cloud/illumos/illumos-drm>. It
sits next to [illumos-ddi](https://code.aopc.cloud/illumos/illumos-ddi),
whose `ddi-sys` and `ddi` crates provide the kernel interfaces, and it uses
the same toolchain, kernel target and build recipe.

The direction was decided on 27 September 2026: a Rust DRM core that
provides the userland interface libdrm and Mesa expect (card and render
nodes, authentication, capabilities, GEM, PRIME, syncobj, KMS), and a Rust
virtio-gpu driver on top of it. Existing vendor drivers stay in C and would
be carried later through a C interface of the core.

## Toolchain

`rust-toolchain.toml` pins stable 1.97.1 with the hosted
`x86_64-unknown-illumos` target for `cargo check`, `clippy` and tests.
Kernel modules are built with nightly-2026-04-16 and `rust-src`, because
`core` and `alloc` have to be rebuilt without SSE for the kernel target in
`targets/x86_64-illumos-kmod.json`.

## Licence

MPL-2.0, see `LICENSE`.

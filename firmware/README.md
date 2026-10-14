# Intel DMC firmware

Unmodified binaries from linux-firmware commit
6569484e6b57bc1c688b383ba6719bf2e25574c4:
https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/6569484e6b57bc1c688b383ba6719bf2e25574c4/

The upstream WHENCE identifies these i915 DMC files under LICENSE.i915.
The complete license is included unchanged and shipped in the IPS license action.
These are binary firmware, not MIT source; do not include them in source licence
whitelists or modify them. No GuC/HuC blobs are supplied.

SHA256:
- i915/skl_dmc_ver1_27.bin: d3b6dc1a39bb2aeb37a1179f2b4e8145c24986da78c533571a5712dc91ec3f61
- i915/kbl_dmc_ver1_04.bin: 2cde41c3e5ad181423bcc3e98ff9c49f743c88f18646af4d0b3c3a9664b831a1
- LICENSE.i915: 8542aeabf2761935122d693561e16766ce1bcc2b0d003204f9040b7d6d929f2e

SKL requests the first file; KBL/CFL/WHL/CML request the second. The driver
retains its INIT power reference if firmware cannot be loaded. Kernel runtime
PM remains constrained by the illumos shim; firmware presence is not evidence
of suspend or runtime power-saving support.

# LinuxKPI2: keep the C boundary, port selectively, reuse Rust internally

Decision recommendation, 2026-10-14: **do not rewrite the Linux-facing layer in
Rust or replace it wholesale with FreeBSD LinuxKPI. Keep its C headers and ABI,
import suitable permissively licensed FreeBSD implementations selectively, and
use the existing Rust core/DDI abstractions behind narrow C interfaces where
ownership is actually enforceable.** Hand-written C is not a legal or technical
requirement. C is the lowest-risk compatibility boundary for the carried C
vendor drivers, not a requirement for every backend implementation.

This is an engineering recommendation, not authorization to start a replacement.
The display/hotplug branches do not depend on accepting it.

## Evidence and scope

The R5c measurement used Linux `8d3ae59288f1e7d58d76558a6ee96d533bc5019f`,
with display/PM/ACPI and several optional facilities disabled: 163 headless
translation units, 113,093 C lines, 437 kernel imports, 63 DRM imports and 39
dma-buf imports. Its 1,292-header closure included 718 GPL-only headers. These
are historical *configuration-specific* measurements, not LinuxKPI2 coverage
or the current display import count. The `r5c-linux` container was a measurement
build environment; it is not a runtime compatibility layer or evidence that
Linux kernel code can execute unchanged inside an illumos kernel.

At i915n stage-1 head `e50b0ce`, the local layer has 41 C source files / 19,226
lines and 284 headers / 15,806 lines (including comments). The native display
build consumes 292 TUs / 791 permitted Linux inputs; the separate KMS module
consumes 41 TUs after the hotplug addition. Line counts are sizing evidence,
not estimates of implementation difficulty. Teardown, memory ordering,
reclaim and interrupt-context semantics dominate the risk.

FreeBSD sample pinned to
[`c30a321a61ce7e1c03d4c314b2127b37f75564c6`](https://github.com/freebsd/freebsd-src/tree/c30a321a61ce7e1c03d4c314b2127b37f75564c6/sys/compat/linuxkpi/common)
(2026-10-14). Reviewed 11 relevant headers and four implementations:
workqueues, RCU, pages, PCI, DMA, firmware, wait queues, mutexes, spinlocks,
xarray and kref. This is a targeted audit, not a full-tree license/API audit.
The separate [FreeBSD drm-kmod tree](https://github.com/freebsd/drm-kmod)
also supplies graphics-specific compatibility such as dma-fence; `sys/compat/linuxkpi`
alone is not a complete drop-in DRM compatibility stack.

## What can be ported

| Area | FreeBSD implementation and illumos work | Recommendation |
| --- | --- | --- |
| Lists, bit operations, reference helpers, small algorithms | Often close to OS-independent C; preserve exact Linux semantics and licenses, compare with our existing implementation | Best candidates when they fix a demonstrated gap; do not churn already-correct code just for provenance |
| Workqueues and delayed work | FreeBSD uses taskqueue/callout, its own state machine and thread context; illumos uses taskq/timeouts and our cancellation/drain rules | Port algorithms only after adversarial cancellation/self-requeue/detach tests; changing names is insufficient |
| RCU, waits, locks | FreeBSD synchronization/critical-section and scheduler assumptions differ from illumos; grace periods and callback lifetime are observable by i915 | Retain one native implementation and test it; avoid mixing two RCU domains accidentally |
| Pages, DMA, mappings | FreeBSD vm_page/pmap and bus DMA are not illumos page_t/HAT/DDI DMA or devmap; cache attributes, scatter-gather lifetime and user faults need native design | High-cost adaptation; keep current native backend until a replacement passes hardware tests |
| PCI, interrupts, firmware, power | FreeBSD device/resource/bus interfaces differ from dev_info_t/DDI; error unwinding, MSI allocation and suspend contracts remain OS-specific | Reuse API shapes and verified logic, not FreeBSD kernel objects |
| dma-fence/resv, GEM, files and minors | Already owned partly by the Rust DRM core and facade; FreeBSD graphics shim expects a different ownership split | Do not import competing object ownership or layouts; bridge to the existing core |
| Runtime PM, ACPI notifications, reclaim and newer GPU memory | Names may exist while behavior is absent or intentionally simplified in either port | A compiled symbol is not implementation coverage; maintain an explicit semantic gap table |

Concrete drift example: the sampled FreeBSD workqueue header supplies
`system_percpu_wq`, but lacks the `disable_work*`/`enable_work*` interfaces present
in our current LinuxKPI header. The current Linux pin and configuration must
be compiled against any imported subset. Neither BSD API breadth nor support
for a particular drm-kmod release proves Linux 7.2 display compatibility.
See the pinned [workqueue header](https://github.com/freebsd/freebsd-src/blob/c30a321a61ce7e1c03d4c314b2127b37f75564c6/sys/compat/linuxkpi/common/include/linux/workqueue.h)
and [page implementation](https://github.com/freebsd/freebsd-src/blob/c30a321a61ce7e1c03d4c314b2127b37f75564c6/sys/compat/linuxkpi/common/src/linux_page.c).

## Licensing

The sampled FreeBSD files carry two-clause BSD-style notices (firmware.h has
an explicit BSD-2-Clause SPDX identifier). Preserve each file's copyright,
conditions and disclaimer, and reproduce required notices with binaries.
A selective port can coexist with the repository's MPL-2.0 Rust crates and
illumos's CDDL files. It does not turn imported BSD code into an exclusively
MPL/CDDL-authored work, and it does not erase obligations on modifications to
existing MPL/CDDL files. Keep imported files and provenance easy to identify.
Sources: [FreeBSD firmware.h](https://github.com/freebsd/freebsd-src/blob/c30a321a61ce7e1c03d4c314b2127b37f75564c6/sys/compat/linuxkpi/common/include/linux/firmware.h),
[MPL FAQ](https://www.mozilla.org/en-US/MPL/2.0/FAQ/),
[illumos CDDL text](https://github.com/illumos/illumos-gate/blob/master/usr/src/OPENSOLARIS.LICENSE).

Do not label the entire FreeBSD tree BSD-2 without auditing it. Review the
transitive headers and copied algorithms too. Our current C glue is largely
MIT, the Rust workspace is MPL-2.0, and Linux source input gates admit MIT/HPND;
GPL-only Linux implementation/header copying remains outside that gate.
A FreeBSD import requires a separate provenance/license inventory and an
explicit build-license policy update, not silently adding BSD to a whitelist
for every Linux file. Firmware redistribution is a separate license issue.

## Rust feasibility and constraints

Rust already runs in this workspace's loaded DRM core and virtio driver;
illumos-ddi has kernel load/unload, DTrace and mdb evidence. External prior art
includes [OPTE's xde driver](https://github.com/oxidecomputer/opte). Therefore
“illumos cannot run Rust kernel code” is false. Conversely, Rust's official
[x86_64-unknown-illumos target](https://doc.rust-lang.org/rustc/platform-support/illumos.html)
is a hosted tier-2 target with std; that status is not a kernel ABI/toolchain
support promise.

The repository uses stable 1.97.1 for hosted checks and pins
nightly-2026-04-16 for kernel core/alloc rebuilds. The custom target disables
SIMD/red-zone, uses the kernel code model, static relocation, frame pointers
and abort-on-panic. Native `ld -ztype=kmod`, dependency/mapfile checks and
forbidden-instruction checks remain necessary. Do not link hosted std or
prebuilt SIMD-capable core into the kernel, unwind through C, or assume a
Rust allocation is legal in interrupt context. Recoverable resource failures
must be fallible, not converted to a kernel panic by infallible allocation.

A C driver directly uses macros, inline functions, GNU expressions, embedded
structures, unions, intrusive lists and callbacks. Rust cannot replace those
headers with a crate. Keep a versioned C facade, `repr(C)` only where needed,
compile-time size/offset/alignment checks in both languages, explicit
ownership/refcount rules, and `extern "C"` callbacks. Rust wrappers must not
create exclusive references while C may alias or mutate the same object.
Pinned addresses and IRQ-safe destruction require deliberate unsafe code;
Rust's type system does not automatically validate the Linux contract.

The strongest Rust candidates are isolated handle tables, lifetime-managed
queues or state machines with opaque C handles. Invasive replacements of
Linux-visible page, spinlock, work_struct or dma_fence layout offer much less
safety leverage and much greater compatibility risk. Existing Rust core
implementations should be reused before introducing a second backend.

## Debuggability

mdb can inspect symbols, memory, registers and disassembly in Rust modules;
DTrace FBT can instrument emitted functions (subject to inlining and ordinary
probe limitations). The existing rnull trial recorded 456 FBT probes. Use
stable unmangled C boundary functions, explicit counters and trace points for
important state transitions; mangled names alone are a poor operational API.

The current DDI design records that ctfconvert does not understand Rust DWARF.
Do not promise `::print`/typed DTrace access to arbitrary Rust enums, generics
or private layouts. Public C mirrors can supply CTF for deliberately stable
layouts, with offset checks; private state may need mdb walkers or explicit
snapshot/debug interfaces. C also needs an actual CTF build pipeline: the
current i915n module itself has no CTF, so merely choosing C does not fix
observability. Preserving frame pointers and checking stack use matter for
both languages. A full Rust rewrite adds a significant debugger/toolchain
maintenance obligation beyond functional driver work.

## Effort and decision gates

Planning ranges below are **engineer-weeks for one experienced kernel engineer**,
including tests/review, not measured delivery promises. Hardware availability,
upstream API drift and crash reproduction can extend calendar time.

| Approach | Initial useful milestone | Credible pilot scope | Main risk |
| --- | --- | --- | --- |
| Harden current C boundary, selective BSD reuse (recommended) | 2–3 weeks for current display import/semantic/license inventory and tests | 8–14 weeks total for selected helpers plus cancellation/RCU/DMA/error-path hardening | Existing semantic shortcuts remain unless tested explicitly |
| Broad FreeBSD LinuxKPI port | 4–6 weeks for an OS-adapter spike and one complete subsystem | 24–40 weeks for current i915/KMS scope; another 12–24 for wider drivers/PM/memory scope | Replacing FreeBSD VM/scheduler/device assumptions while reconciling our Rust ownership split |
| Rust backend behind retained C headers | 3–5 weeks for one opaque, fallible ownership subsystem with CTF/debug plan | 12–24 weeks for several selected backends | Unsafe FFI aliases, IRQ destruction and duplicate abstractions |
| Mostly Rust replacement of current LinuxKPI | 6–10 weeks to establish ABI/toolchain/debug/test infrastructure | 32–52+ weeks for current scope; C headers still remain | Huge rewrite verification burden, no automatic removal of C-driver unsafety |

Start with a two-week bounded comparison, not a rewrite: regenerate imports
for display and headless, classify each exported API as implemented/simplified/
stub/refused, pin the FreeBSD candidate set, and choose one demonstrably weak
subsystem. Acceptance requires concurrent cancel/requeue/unregister, allocation
failure, interrupt-context and memory-order tests plus exact-image guest and
X230 regression. Gen9/other hardware remains a separate validation gate.

No language prototype was added: the existing Rust DRM core already proves
kernel execution and FFI feasibility, while a toy replacement would not answer
workqueue/VM/RCU correctness. A useful next prototype is a differential
cancellation/lifetime test harness for the chosen subsystem, followed by a
single optional backend that can be compared with the current implementation.

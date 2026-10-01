// Vendored from illumos-ddi xtask/src/main.rs at eb18ab93b7
// (https://code.aopc.cloud/illumos/illumos-ddi). Changes against upstream:
//
// * `deps` reads the full `cargo metadata` graph instead of `--no-deps`,
//   because `ddi-sys` and `ddi` come from a git dependency and their
//   `[package.metadata.kmod]` tables are not workspace members here.
// * `kernel-check` scans the `-sys` crates of that graph (see
//   `kernel_check.rs`).
// * `lint-man` is not vendored: this workspace has no `-sys` crate of its
//   own, and illumos-ddi runs it on `ddi-sys`.
// * `uapi-dump` and `uapi-check` are new and belong to this repository.
// * `build` accepts `--features <list>`, for the test-only `panic-drill`
//   feature of rdrmnull.
// * `link` reads an `exports` mapfile from the package's
//   `[package.metadata.kmod]` table and links with `-B reduce -M <file>`,
//   so that a module other modules depend on (misc/drm) exports only
//   the symbols the mapfile names; `deps` prints it.
// * `local-mapfiles` in a `[package.metadata.kmod]` table name mapfiles of
//   this workspace (relative to its root), used like the gate's mapfiles
//   with `-z defs`; `deps` drops a package's own module from its `-N`
//   list (misc/drm uses drm-facade-sys for its types).
// * `facade-dump` and `facade-check` are new (the DRM facade layouts), as
//   is `symbols`, the symbol-scope check of a linked module.
//
// Keep build, link, deps and kernel-check in step with upstream when the
// pinned illumos-ddi revision moves.

//! Builds and links the DRM kernel modules and checks their interfaces.
//!
//! ```text
//! cargo xtask build <pkg> [--debug] [--features <list>]  # kernel target, pinned nightly, -Zbuild-std
//! cargo xtask link  <pkg>            # on illumos: ld -ztype=kmod
//! cargo xtask deps  <pkg>            # -N list and mapfiles implied by the crate graph
//! cargo xtask kernel-check --emit | ssh root@host mdb -k | cargo xtask kernel-check --verify -
//! cargo xtask uapi-dump              # sizes, offsets and ioctl numbers of drm-uapi
//! cargo xtask uapi-check <file>      # compare with the output of tools/uapi/drm_layout.c
//! cargo xtask facade-dump            # sizes and offsets of drm-facade-sys
//! cargo xtask facade-check <file>    # compare with tools/facade/facade_layout.c (gcc -S)
//! cargo xtask symbols <module> <mapfile>  # on illumos: global symbols = the mapfile's
//! ```
//!
//! The `-N` dependencies and the mapfile names are collected from the
//! `[package.metadata.kmod]` table of every crate in the dependency graph,
//! as in illumos-ddi (section 6 of its `docs/DESIGN.md`).

mod facade_table;
mod kernel_check;
mod uapi_compat;
mod uapi_table;

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

const KMOD_TARGET: &str = "targets/x86_64-illumos-kmod.json";
// Pinned because -Zbuild-std tracks nightly closely; bump deliberately.
const NIGHTLY: &str = "nightly-2026-04-16";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let r = match args.as_slice() {
        [cmd, pkg] if cmd == "build" => build(pkg, true, None),
        [cmd, pkg, dbg] if cmd == "build" && dbg == "--debug" => build(pkg, false, None),
        [cmd, pkg, f, list] if cmd == "build" && f == "--features" => build(pkg, true, Some(list)),
        [cmd, pkg, dbg, f, list] if cmd == "build" && dbg == "--debug" && f == "--features" => {
            build(pkg, false, Some(list))
        }
        [cmd, pkg] if cmd == "link" => link(pkg),
        [cmd, pkg] if cmd == "deps" => deps(pkg).map(|d| {
            println!("-N: {:?}", d.needed);
            println!("mapfiles: {:?}", d.mapfiles);
            println!("local mapfiles: {:?}", d.local_mapfiles);
            println!("class: {}", d.class);
            println!("exports: {:?}", d.exports);
        }),
        [cmd, rest @ ..] if cmd == "kernel-check" => kernel_check::run(rest),
        [cmd] if cmd == "uapi-dump" => {
            for l in uapi_table::lines() {
                println!("{l}");
            }
            Ok(())
        }
        [cmd, file] if cmd == "uapi-check" => uapi_check(file),
        [cmd] if cmd == "facade-dump" => {
            for l in facade_table::lines() {
                println!("{l}");
            }
            Ok(())
        }
        [cmd, file] if cmd == "facade-check" => facade_check(file),
        [cmd, module, mapfile] if cmd == "symbols" => symbols(module, mapfile),
        _ => Err("usage: cargo xtask (build [--debug] [--features LIST]|link|deps) <package> \
                  | kernel-check --emit|--verify FILE \
                  | uapi-dump | uapi-check FILE | facade-dump | facade-check FILE \
                  | symbols MODULE MAPFILE"
            .into()),
    };
    match r {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("xtask: {e}");
            ExitCode::FAILURE
        }
    }
}

struct KmodDeps {
    needed: BTreeSet<String>,
    mapfiles: BTreeSet<String>,
    /// Mapfiles of this workspace (`local-mapfiles`), for `-z defs`.
    local_mapfiles: BTreeSet<PathBuf>,
    class: String,
    conf: Option<PathBuf>,
    /// The package's own symbol-scope mapfile (`exports`), if it has one.
    exports: Option<PathBuf>,
}

fn workspace_root() -> PathBuf {
    let out = Command::new("cargo")
        .args(["locate-project", "--workspace", "--message-format=plain"])
        .output()
        .expect("cargo");
    let s = String::from_utf8_lossy(&out.stdout);
    Path::new(s.trim()).parent().expect("workspace root").to_path_buf()
}

fn metadata() -> Result<serde_json::Value, String> {
    let out = Command::new("cargo")
        .args(["metadata", "--format-version=1"])
        .current_dir(workspace_root())
        .output()
        .map_err(|e| e.to_string())?;
    if !out.status.success() {
        return Err(format!("cargo metadata failed: {}", String::from_utf8_lossy(&out.stderr)));
    }
    serde_json::from_slice(&out.stdout).map_err(|e| e.to_string())
}

/// Returns the `-sys` crates with a `[package.metadata.kmod]` table in the
/// dependency graph, as `(package name, crate directory)`.
fn sys_crates() -> Result<Vec<(String, PathBuf)>, String> {
    let meta = metadata()?;
    let mut out = Vec::new();
    for p in meta["packages"].as_array().ok_or("no packages")? {
        let name = p["name"].as_str().unwrap_or_default();
        if !name.ends_with("-sys") || p["metadata"]["kmod"].is_null() {
            continue;
        }
        let dir = Path::new(p["manifest_path"].as_str().unwrap_or_default()).parent().ok_or("manifest")?;
        out.push((name.to_string(), dir.to_path_buf()));
    }
    out.sort();
    Ok(out)
}

/// Reads `cargo metadata` and merges the `[package.metadata.kmod]` tables of
/// every crate that `pkg` depends on, directly or not, into one set of
/// dependencies and mapfiles.
fn deps(pkg: &str) -> Result<KmodDeps, String> {
    let meta = metadata()?;
    let packages = meta["packages"].as_array().ok_or("no packages")?;
    let by_id: BTreeMap<&str, &serde_json::Value> =
        packages.iter().map(|p| (p["id"].as_str().unwrap_or_default(), p)).collect();
    let root = packages.iter().find(|p| p["name"] == pkg).ok_or(format!("package {pkg} not found"))?;
    // Walk the resolved graph from the package, so that only crates it
    // actually links contribute -N flags and mapfiles.
    let nodes: BTreeMap<&str, Vec<&str>> = meta["resolve"]["nodes"]
        .as_array()
        .ok_or("no resolve graph")?
        .iter()
        .map(|n| {
            let deps = n["dependencies"].as_array().into_iter().flatten().filter_map(|d| d.as_str()).collect();
            (n["id"].as_str().unwrap_or_default(), deps)
        })
        .collect();
    let mut seen = BTreeSet::new();
    let mut stack = vec![root["id"].as_str().unwrap_or_default()];
    while let Some(id) = stack.pop() {
        if seen.insert(id) {
            stack.extend(nodes.get(id).into_iter().flatten().copied());
        }
    }
    let mut d = KmodDeps {
        needed: BTreeSet::new(),
        mapfiles: BTreeSet::new(),
        local_mapfiles: BTreeSet::new(),
        class: "drv".into(),
        conf: None,
        exports: None,
    };
    for id in seen {
        let Some(p) = by_id.get(id) else { continue };
        let kmod = &p["metadata"]["kmod"];
        if kmod.is_null() {
            continue;
        }
        for n in kmod["depends"].as_array().into_iter().flatten() {
            d.needed.insert(n.as_str().unwrap_or_default().to_string());
        }
        for m in kmod["mapfiles"].as_array().into_iter().flatten() {
            d.mapfiles.insert(m.as_str().unwrap_or_default().to_string());
        }
        for m in kmod["local-mapfiles"].as_array().into_iter().flatten() {
            d.local_mapfiles.insert(workspace_root().join(m.as_str().unwrap_or_default()));
        }
        if p["name"] == pkg {
            if let Some(c) = kmod["class"].as_str() {
                d.class = c.to_string();
            }
            let dir = Path::new(p["manifest_path"].as_str().unwrap_or_default()).parent().ok_or("manifest")?;
            if let Some(c) = kmod["conf"].as_str() {
                d.conf = Some(dir.join(c));
            }
            if let Some(m) = kmod["exports"].as_str() {
                d.exports = Some(dir.join(m));
            }
        }
    }
    // A module does not depend on itself (misc/drm uses drm-facade-sys,
    // which names misc/drm, for the facade's types).
    let own = format!("{}/{}", d.class, pkg);
    d.needed.remove(&own);
    Ok(d)
}

fn build(pkg: &str, release: bool, features: Option<&String>) -> Result<(), String> {
    let root = workspace_root();
    let mut cmd = Command::new("cargo");
    cmd.current_dir(&root)
        .arg(format!("+{NIGHTLY}"))
        .args(["build", "-p", pkg, "--target"])
        .arg(root.join(KMOD_TARGET))
        .args(["-Zjson-target-spec", "-Zbuild-std=core,alloc", "-Zbuild-std-features=compiler-builtins-mem"])
        .env("RUSTFLAGS", "-C symbol-mangling-version=v0 -C force-frame-pointers=yes");
    if release {
        cmd.arg("--release");
    }
    if let Some(f) = features {
        cmd.args(["--features", f]);
    }
    println!("{cmd:?}");
    let st = cmd.status().map_err(|e| e.to_string())?;
    if !st.success() {
        return Err("cargo build failed".into());
    }
    println!("archive: target/x86_64-illumos-kmod/{}/{pkg}.a", if release { "release" } else { "debug" });
    Ok(())
}

fn link(pkg: &str) -> Result<(), String> {
    if std::env::consts::OS != "illumos" {
        return Err("link needs the illumos ld(1); run this step on an illumos host".into());
    }
    let root = workspace_root();
    let d = deps(pkg)?;
    let archive = root.join(format!("target/x86_64-illumos-kmod/release/{pkg}.a"));
    let out = root.join(format!("target/x86_64-illumos-kmod/release/{pkg}"));
    let mut cmd = Command::new("ld");
    cmd.arg("-ztype=kmod");
    for n in &d.needed {
        cmd.arg(format!("-N{n}"));
    }
    if let Ok(gate) = std::env::var("ILLUMOS_GATE") {
        cmd.args(["-z", "defs"]);
        for m in &d.mapfiles {
            cmd.arg("-M").arg(format!("{gate}/usr/src/uts/common/mapfiles/{m}.mapfile"));
        }
        for m in &d.local_mapfiles {
            cmd.arg("-M").arg(m);
        }
    }
    if let Some(m) = &d.exports {
        // Everything the mapfile does not name becomes local.
        cmd.args(["-B", "reduce", "-M"]).arg(m);
    }
    cmd.args(["-z", "allextract"]).arg(&archive).arg("-o").arg(&out);
    println!("{cmd:?}");
    let st = cmd.status().map_err(|e| e.to_string())?;
    if !st.success() {
        return Err("ld failed".into());
    }
    println!("kernel module: {}", out.display());
    if let Some(c) = d.conf {
        println!(
            "install the conf file {} as /usr/kernel/drv/{pkg}.conf and the module as /usr/kernel/{}/amd64/{pkg}",
            c.display(),
            d.class
        );
    } else {
        println!("install the module as /usr/kernel/{}/amd64/{pkg} (or /kernel/{}/amd64/{pkg})", d.class, d.class);
    }
    Ok(())
}

/// Compares the output of `tools/uapi/drm_layout.c` and of
/// `tools/uapi/virtgpu_compat_layout.c` (gcc on illumos, concatenated) with
/// the same facts computed from the Rust definitions, line by line. Every
/// line must be present on both sides with the same value.
fn uapi_check(file: &str) -> Result<(), String> {
    let rust: Vec<String> = uapi_table::lines().into_iter().chain(uapi_compat::lines()).collect();
    layout_check("uapi-check", file, &rust)
}

/// Compares the output of `tools/facade/facade_layout.c` (gcc -S on
/// illumos, the `@@` lines) with the sizes and offsets of drm-facade-sys.
fn facade_check(file: &str) -> Result<(), String> {
    layout_check("facade-check", file, &facade_table::lines())
}

/// Compares `key value` lines from gcc with the same lines from Rust; every
/// line must be present on both sides with the same value.
fn layout_check(what: &str, file: &str, rust_lines: &[String]) -> Result<(), String> {
    let c_text = std::fs::read_to_string(file).map_err(|e| format!("{file}: {e}"))?;
    let split = |l: &str| -> Option<(String, String)> {
        let (key, value) = l.trim().rsplit_once(' ')?;
        Some((key.to_string(), value.to_string()))
    };
    let c: BTreeMap<String, String> = c_text.lines().filter_map(split).collect();
    let rust: BTreeMap<String, String> = rust_lines.iter().filter_map(|l| split(l)).collect();
    let mut errors = 0;
    let mut counts: BTreeMap<&str, usize> = BTreeMap::new();
    for (key, rv) in &rust {
        match c.get(key) {
            Some(cv) if cv == rv => *counts.entry(key.split(' ').next().unwrap_or("?")).or_default() += 1,
            Some(cv) => {
                errors += 1;
                eprintln!("mismatch: {key}: Rust {rv}, gcc {cv}");
            }
            None => {
                errors += 1;
                eprintln!("missing in gcc output: {key}");
            }
        }
    }
    for key in c.keys().filter(|k| !rust.contains_key(*k)) {
        errors += 1;
        eprintln!("missing in Rust: {key}");
    }
    println!(
        "{what}: {} sizes, {} offsets, {} ioctl numbers identical; {} lines compared, {errors} differences",
        counts.get("size").copied().unwrap_or(0),
        counts.get("off").copied().unwrap_or(0) + counts.get("offset").copied().unwrap_or(0),
        counts.get("ioctl").copied().unwrap_or(0),
        rust.len()
    );
    if errors > 0 { Err(format!("{what}: {errors} layout differences")) } else { Ok(()) }
}

/// The names a symbol-scope mapfile makes global (`global:` up to `local:`).
fn mapfile_globals(text: &str) -> BTreeSet<String> {
    let mut out = BTreeSet::new();
    let mut global = false;
    for l in text.lines() {
        let l = l.split('#').next().unwrap_or("").trim();
        if l.starts_with("global:") {
            global = true;
        } else if l.starts_with("local:") {
            global = false;
        } else if global && let Some(name) = l.strip_suffix(';') {
            let name = name.split_whitespace().next().unwrap_or("");
            if !name.is_empty() {
                out.insert(name.to_string());
            }
        }
    }
    out
}

/// Checks a linked module's symbol scope: its global defined symbols must
/// be exactly the mapfile's global names (on illumos, with elfdump(1)).
fn symbols(module: &str, mapfile: &str) -> Result<(), String> {
    let want = mapfile_globals(&std::fs::read_to_string(mapfile).map_err(|e| format!("{mapfile}: {e}"))?);
    let out = Command::new("elfdump").args(["-s", "-N", ".symtab", module]).output().map_err(|e| e.to_string())?;
    if !out.status.success() {
        return Err(format!("elfdump {module} failed"));
    }
    // [Index]  Value  Size  Type  Bind  Other Ver Shndx  Name
    let mut have = BTreeSet::new();
    for l in String::from_utf8_lossy(&out.stdout).lines() {
        let f: Vec<&str> = l.split_whitespace().collect();
        if f.len() >= 9 && f[0].starts_with('[') && f[4] == "GLOB" && f[7] != "UNDEF" {
            have.insert(f[8].to_string());
        }
    }
    let extra: Vec<_> = have.difference(&want).collect();
    let missing: Vec<_> = want.difference(&have).collect();
    for s in &extra {
        eprintln!("global but not in the mapfile: {s}");
    }
    for s in &missing {
        eprintln!("in the mapfile but not defined: {s}");
    }
    println!("symbols: {} global symbols, {} extra, {} missing", have.len(), extra.len(), missing.len());
    if extra.is_empty() && missing.is_empty() { Ok(()) } else { Err("symbol scope differs from the mapfile".into()) }
}

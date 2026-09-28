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
//! ```
//!
//! The `-N` dependencies and the mapfile names are collected from the
//! `[package.metadata.kmod]` table of every crate in the dependency graph,
//! as in illumos-ddi (section 6 of its `docs/DESIGN.md`).

mod kernel_check;
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
            println!("class: {}", d.class);
        }),
        [cmd, rest @ ..] if cmd == "kernel-check" => kernel_check::run(rest),
        [cmd] if cmd == "uapi-dump" => {
            for l in uapi_table::lines() {
                println!("{l}");
            }
            Ok(())
        }
        [cmd, file] if cmd == "uapi-check" => uapi_check(file),
        _ => Err("usage: cargo xtask (build [--debug] [--features LIST]|link|deps) <package> \
                  | kernel-check --emit|--verify FILE \
                  | uapi-dump | uapi-check FILE"
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
    class: String,
    conf: Option<PathBuf>,
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
    let mut d = KmodDeps { needed: BTreeSet::new(), mapfiles: BTreeSet::new(), class: "drv".into(), conf: None };
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
        if p["name"] == pkg {
            if let Some(c) = kmod["class"].as_str() {
                d.class = c.to_string();
            }
            if let Some(c) = kmod["conf"].as_str() {
                let dir = Path::new(p["manifest_path"].as_str().unwrap_or_default()).parent().ok_or("manifest")?;
                d.conf = Some(dir.join(c));
            }
        }
    }
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
    }
    Ok(())
}

/// Compares the output of `tools/uapi/drm_layout.c` (gcc on illumos) with
/// the same facts computed from the Rust definitions, line by line. Every
/// line must be present on both sides with the same value.
fn uapi_check(file: &str) -> Result<(), String> {
    let c_text = std::fs::read_to_string(file).map_err(|e| format!("{file}: {e}"))?;
    let split = |l: &str| -> Option<(String, String)> {
        let (key, value) = l.trim().rsplit_once(' ')?;
        Some((key.to_string(), value.to_string()))
    };
    let c: BTreeMap<String, String> = c_text.lines().filter_map(split).collect();
    let rust: BTreeMap<String, String> = uapi_table::lines().iter().filter_map(|l| split(l)).collect();
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
        "uapi-check: {} sizes, {} offsets, {} ioctl numbers identical; {} lines compared, {errors} differences",
        counts.get("size").copied().unwrap_or(0),
        counts.get("off").copied().unwrap_or(0),
        counts.get("ioctl").copied().unwrap_or(0),
        rust.len()
    );
    if errors > 0 { Err(format!("{errors} uAPI layout differences")) } else { Ok(()) }
}

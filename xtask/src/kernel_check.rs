// Vendored from illumos-ddi xtask/src/kernel_check.rs at eb18ab93b7
// (https://code.aopc.cloud/illumos/illumos-ddi). Change: the `-sys` crates
// are found in the dependency graph through `cargo metadata` (they come from
// the illumos-ddi git dependency) instead of under `crates/` of this
// workspace. Keep it in step with upstream when bumping the illumos-ddi rev.

//! Checks the `-sys` crates against a running illumos kernel.
//!
//! The check has two halves and never needs the gate source. The first half
//! compares every `size_of` and `offset_of!` assertion in the `-sys` crates
//! with the CTF type data of the running kernel. The second half asks the
//! kernel where each declared function and static lives, and reports any
//! that the kernel does not export at all, as well as any that resolve from
//! a module the crate does not declare in its `[package.metadata.kmod]`
//! table.
//!
//! The tool does not log in anywhere itself. It writes an `mdb(1)` script,
//! the kernel host runs it, and the tool reads the output back:
//!
//! ```text
//! cargo xtask kernel-check --emit > check.mdb
//! ssh root@host mdb -k < check.mdb > check.out
//! cargo xtask kernel-check --verify check.out
//! ```
//!
//! The two steps can also be joined in one pipe through `ssh`.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};

/// Rust field names that differ from the C member name, as
/// `(type, rust field, C member)`. Rust keywords get a trailing underscore
/// and is stripped automatically; this table covers the other cases.
const FIELD_ALIASES: &[(&str, &str, &str)] = &[("uio", "uio_loffset", "_uio_offset")];

/// Rust fields that hold C bit-fields as whole bytes, as `(type, field)`.
/// Rust has no bit-fields, so these bytes have no C member name that `mdb`
/// could look up; the size of the containing structure and the offsets of
/// the neighbouring named fields still pin them.
const BITFIELD_STORAGE: &[(&str, &str)] = &[
    ("scsi_device", "sd_bits"),
    ("scsi_extended_sense", "es_byte0"),
    ("scsi_extended_sense", "es_byte2"),
    ("scsi_g0", "control"),
    ("scsi_g1", "control"),
    ("scsi_g4", "control"),
    ("scsi_g5", "control"),
    ("scsi_inquiry", "inq_byte56"),
    ("scsi_inquiry", "inq_byte7"),
    ("scsi_status", "sts_byte0"),
];

/// Symbols that the gate added after a kernel the check is commonly run
/// against, as `(symbol, gate commit and date)`. A missing symbol from this
/// list is reported as a note, not as an error.
const NEWER_THAN_KERNEL: &[(&str, &str)] = &[("ddi_ncpus_expected", "2fc1284f54, 4 August 2026")];

/// Modules every crate may resolve symbols from, because they are always
/// loaded and hold the base kernel.
const BASE_MODULES: &[&str] = &["genunix", "unix"];

struct Crate {
    name: String,
    modules: BTreeSet<String>,
    sizes: BTreeMap<String, u64>,
    offsets: BTreeSet<(String, String, u64)>,
    symbols: BTreeSet<String>,
}

pub fn run(args: &[String]) -> Result<(), String> {
    let crates = scan(&super::sys_crates()?)?;
    match args {
        [flag] if flag == "--emit" => {
            print!("{}", emit(&crates));
            Ok(())
        }
        [flag, file] if flag == "--verify" => {
            let mut text = String::new();
            if file == "-" {
                std::io::stdin().read_to_string(&mut text).map_err(|e| e.to_string())?;
            } else {
                text = fs::read_to_string(file).map_err(|e| format!("{file}: {e}"))?;
            }
            verify(&crates, &text)
        }
        _ => Err("usage: cargo xtask kernel-check --emit | --verify <mdb output or ->".into()),
    }
}

/// Scans the given `-sys` crates, as `(package name, crate directory)`.
fn scan(sys_crates: &[(String, PathBuf)]) -> Result<Vec<Crate>, String> {
    let mut out = Vec::new();
    for (name, path) in sys_crates {
        let name = name.clone();
        let manifest = fs::read_to_string(path.join("Cargo.toml")).map_err(|e| e.to_string())?;
        let mut modules: BTreeSet<String> = BASE_MODULES.iter().map(|s| s.to_string()).collect();
        if let Some(line) = manifest.lines().find(|l| l.trim_start().starts_with("depends")) {
            for dep in line.split('"').skip(1).step_by(2) {
                modules.insert(dep.rsplit('/').next().unwrap_or(dep).to_string());
            }
        }
        let mut c = Crate { name, modules, sizes: BTreeMap::new(), offsets: BTreeSet::new(), symbols: BTreeSet::new() };
        let mut files = Vec::new();
        collect_rs(&path.join("src"), &mut files)?;
        for f in files {
            let text = fs::read_to_string(&f).map_err(|e| e.to_string())?;
            scan_source(&text, &mut c);
        }
        out.push(c);
    }
    Ok(out)
}

fn collect_rs(dir: &Path, out: &mut Vec<PathBuf>) -> Result<(), String> {
    for e in fs::read_dir(dir).map_err(|e| format!("{}: {e}", dir.display()))? {
        let p = e.map_err(|e| e.to_string())?.path();
        if p.is_dir() {
            collect_rs(&p, out)?;
        } else if p.extension().and_then(|x| x.to_str()) == Some("rs") {
            out.push(p);
        }
    }
    Ok(())
}

/// Evaluates the small constant expressions the assertions use, such as
/// `13 * 8 + 8` or `0x58`.
fn eval(expr: &str) -> Option<u64> {
    let mut total = 0u64;
    for term in expr.split('+') {
        let mut prod = 1u64;
        for f in term.split('*') {
            let f = f.trim();
            let v =
                if let Some(h) = f.strip_prefix("0x") { u64::from_str_radix(h, 16).ok()? } else { f.parse().ok()? };
            prod = prod.checked_mul(v)?;
        }
        total = total.checked_add(prod)?;
    }
    Some(total)
}

fn scan_source(text: &str, c: &mut Crate) {
    let mut in_extern = false;
    for line in text.lines() {
        let t = line.trim();
        if t.starts_with("unsafe extern \"C\"") || t.starts_with("extern \"C\"") {
            in_extern = true;
        } else if in_extern && t == "}" && !line.starts_with(' ') {
            in_extern = false;
        }
        if in_extern {
            for kw in ["pub fn ", "pub static mut ", "pub static "] {
                if let Some(rest) = t.strip_prefix(kw) {
                    let end = rest.find(|ch: char| !(ch.is_ascii_alphanumeric() || ch == '_')).unwrap_or(rest.len());
                    c.symbols.insert(rest[..end].to_string());
                }
            }
        }
        if let Some(i) = t.find("size_of::<")
            && let Some((ty, rest)) = t[i + 10..].split_once(">()")
            && let Some(expr) = rest.trim_start().strip_prefix("==").and_then(|r| r.split(')').next())
            && let Some(v) = eval(expr)
        {
            c.sizes.insert(ty.to_string(), v);
        }
        if let Some(i) = t.find("offset_of!(")
            && let Some((args, rest)) = t[i + 11..].split_once(')')
            && let Some((ty, field)) = args.split_once(',')
            && let Some(expr) = rest.trim_start().strip_prefix("==").and_then(|r| r.split(')').next())
            && let Some(v) = eval(expr)
        {
            c.offsets.insert((ty.trim().to_string(), field.trim().to_string(), v));
        }
    }
}

/// The spellings under which `mdb` might know a Rust type name, in order.
fn spellings(ty: &str) -> Vec<String> {
    let base = ty.strip_suffix("_t").unwrap_or(ty);
    let mut v = vec![
        format!("struct {ty}"),
        ty.to_string(),
        format!("{ty}_t"),
        format!("struct {base}"),
        format!("union {ty}"),
    ];
    v.dedup();
    v
}

fn c_member(ty: &str, field: &str) -> String {
    if let Some((_, _, c)) = FIELD_ALIASES.iter().find(|(t, f, _)| *t == ty && *f == field) {
        return c.to_string();
    }
    field.strip_suffix('_').unwrap_or(field).to_string()
}

fn emit(crates: &[Crate]) -> String {
    let mut s = String::new();
    for c in crates {
        for ty in c.sizes.keys() {
            for sp in spellings(ty) {
                s += &format!("::echo '@@size {} {ty} [{sp}]'\n::sizeof {sp}\n", c.name);
            }
        }
        for (ty, field, _) in &c.offsets {
            for sp in spellings(ty) {
                s += &format!(
                    "::echo '@@off {} {ty} {field} [{sp}]'\n::offsetof {sp} {}\n",
                    c.name,
                    c_member(ty, field)
                );
            }
        }
        for sym in &c.symbols {
            s += &format!("::echo '@@sym {} {sym}'\n{sym}::whatis\n", c.name);
        }
    }
    s
}

fn parse_num(s: &str) -> Option<u64> {
    let s = s.trim();
    if let Some(h) = s.strip_prefix("0x") { u64::from_str_radix(h, 16).ok() } else { s.parse().ok() }
}

fn verify(crates: &[Crate], text: &str) -> Result<(), String> {
    // (kind, crate, key) -> first successful answer
    let mut sizes: BTreeMap<(String, String), u64> = BTreeMap::new();
    let mut offsets: BTreeMap<(String, String, String), u64> = BTreeMap::new();
    let mut syms: BTreeMap<(String, String), Option<String>> = BTreeMap::new();
    let mut cur: Vec<String> = Vec::new();
    for line in text.lines() {
        if let Some(rest) = line.strip_prefix("@@") {
            cur = rest.split_whitespace().map(str::to_string).collect();
            continue;
        }
        match cur.first().map(String::as_str) {
            Some("size") if line.starts_with("sizeof (") => {
                if let Some(v) = line.rsplit_once(" = ").and_then(|(_, v)| parse_num(v)) {
                    sizes.entry((cur[1].clone(), cur[2].clone())).or_insert(v);
                }
            }
            Some("off") if line.starts_with("offsetof (") => {
                let v = line.split(") = ").nth(1).and_then(|r| r.split(',').next()).and_then(parse_num);
                if let Some(v) = v {
                    offsets.entry((cur[1].clone(), cur[2].clone(), cur[3].clone())).or_insert(v);
                }
            }
            Some("sym") => {
                let key = (cur[1].clone(), cur[2].clone());
                let module = line.split(", in ").nth(1).and_then(|r| r.split('\'').next()).map(str::to_string);
                if line.contains(" is ") && module.is_some() {
                    syms.insert(key, module);
                } else {
                    syms.entry(key).or_insert(None);
                }
            }
            _ => {}
        }
    }

    let mut errors = 0;
    for c in crates {
        let (mut s_ok, mut o_ok, mut y_ok) = (0, 0, 0);
        let mut not_in_ctf = Vec::new();
        let mut bitfields = 0;
        for (ty, want) in &c.sizes {
            match sizes.get(&(c.name.clone(), ty.clone())) {
                Some(got) if got == want => s_ok += 1,
                Some(got) => {
                    errors += 1;
                    eprintln!("{}: size of {ty} is {want} in Rust but {got} in the kernel", c.name);
                }
                None => not_in_ctf.push(ty.as_str()),
            }
        }
        for (ty, field, want) in &c.offsets {
            match offsets.get(&(c.name.clone(), ty.clone(), field.clone())) {
                Some(got) if got == want => o_ok += 1,
                Some(got) => {
                    errors += 1;
                    eprintln!("{}: offset of {ty}.{field} is {want} in Rust but {got} in the kernel", c.name);
                }
                None if not_in_ctf.contains(&ty.as_str()) => {}
                None if BITFIELD_STORAGE.contains(&(ty.as_str(), field.as_str())) => bitfields += 1,
                None => {
                    errors += 1;
                    eprintln!("{}: the kernel has no member {} in {ty}", c.name, c_member(ty, field));
                }
            }
        }
        for sym in &c.symbols {
            match syms.get(&(c.name.clone(), sym.clone())) {
                Some(Some(m)) if c.modules.contains(m) => y_ok += 1,
                Some(Some(m)) => {
                    errors += 1;
                    eprintln!("{}: {sym} resolves from module {m}, which the crate does not declare", c.name);
                }
                Some(None) => match NEWER_THAN_KERNEL.iter().find(|(n, _)| n == sym) {
                    Some((_, since)) => {
                        eprintln!("{}: note: {sym} was added to the gate in {since}; this kernel predates it", c.name)
                    }
                    None => {
                        errors += 1;
                        eprintln!("{}: the running kernel does not export {sym}", c.name);
                    }
                },
                None => {
                    errors += 1;
                    eprintln!("{}: no answer for {sym}; was the output produced by --emit for this tree?", c.name);
                }
            }
        }
        println!(
            "{}: sizes {s_ok}/{}, offsets {o_ok}/{}{}, symbols {y_ok}/{}{}",
            c.name,
            c.sizes.len(),
            c.offsets.len(),
            if bitfields > 0 { format!(" ({bitfields} bit-field bytes without a C name)") } else { String::new() },
            c.symbols.len(),
            if not_in_ctf.is_empty() {
                String::new()
            } else {
                format!("; anonymous in CTF, checked through their parents: {}", not_in_ctf.join(" "))
            }
        );
    }
    if errors > 0 { Err(format!("{errors} kernel-check errors")) } else { Ok(()) }
}

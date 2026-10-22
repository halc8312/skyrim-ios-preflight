//! Explicit import handling policy. Diagnostic traps never return a stub value.
use std::collections::{HashMap, HashSet};

use crate::dispatch::{ImportName, ModuleId};
use crate::variables::Variables;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ImportPolicy {
    /// Historical host behavior, retained by Kernel::new.
    #[default]
    LegacyStub,
    /// Refuse any missing function or variable before kernel allocations.
    StrictAll,
    /// Refuse missing variables; missing functions become fatal diagnostic traps.
    CpuDiagnosticTrap,
}

#[derive(Debug, Clone)]
pub struct MissingImport {
    pub library: String,
    pub module: ModuleId,
    pub ordinal: u16,
    pub name: Option<&'static str>,
}

#[derive(Debug, Clone, Default)]
pub struct ImportPreflight {
    pub function_imports: usize,
    pub registered_functions: usize,
    pub missing_functions: Vec<MissingImport>,
    pub variable_imports: usize,
    pub supported_variables: usize,
    pub missing_variables: Vec<MissingImport>,
}

/// Typed Rust unwind payload. The guest thread boundary treats it as failure;
/// it is not ThreadExit, an NTSTATUS return, or a successful emulation stub.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct UnsupportedImport {
    pub module: ModuleId,
    pub ordinal: u16,
}

static DIAGNOSTIC_FATAL: std::sync::OnceLock<fn(UnsupportedImport) -> !> = std::sync::OnceLock::new();

/// Process-lifetime diagnostic reporter. Its noreturn contract prevents helper
/// catch_unwind boundaries from turning an unsupported import into continuation.
pub fn install_diagnostic_failure_handler(handler: fn(UnsupportedImport) -> !) -> Result<(), &'static str> {
    DIAGNOSTIC_FATAL.set(handler).map_err(|_| "diagnostic failure handler already installed")
}

pub(crate) fn fail_unsupported(policy: ImportPolicy, module: ModuleId, ordinal: u16) -> ! {
    let failure = UnsupportedImport { module, ordinal };
    if policy == ImportPolicy::CpuDiagnosticTrap {
        if let Some(handler) = DIAGNOSTIC_FATAL.get() {
            // `-> !` also permits unwinding. A faulty reporter must not escape
            // into a helper's catch_unwind boundary and resume guest execution.
            if let Err(payload) = std::panic::catch_unwind(|| -> () { handler(failure) }) {
                // A user-defined panic payload may itself panic while dropping.
                std::mem::forget(payload);
            }
        }
        // Even without a reporter, a diagnostic trap must kill the process;
        // a GPU/audio helper's generic catch boundary cannot swallow it.
        std::process::abort();
    }
    std::panic::resume_unwind(Box::new(failure))
}

impl ImportPreflight {
    pub fn validate(&self, policy: ImportPolicy) -> Result<(), String> {
        if policy == ImportPolicy::LegacyStub {
            return Ok(());
        }
        if !self.missing_variables.is_empty() {
            return Err(format!(
                "strict import policy refuses {} unsupported variable imports before allocation",
                self.missing_variables.len()
            ));
        }
        if policy == ImportPolicy::StrictAll && !self.missing_functions.is_empty() {
            return Err(format!(
                "strict import policy refuses {} unsupported function imports",
                self.missing_functions.len()
            ));
        }
        Ok(())
    }
}

pub fn preflight_imports(xex: &xex::Xex, names: &[ImportName]) -> Result<ImportPreflight, String> {
    let libraries: Vec<String> =
        xex.import_libraries().map_err(|e| e.to_string())?.into_iter().map(|l| l.name).collect();
    let imports = xex.imports().map_err(|e| e.to_string())?;
    preflight_records(&libraries, &imports, names)
}

fn preflight_records(
    libraries: &[String],
    imports: &[xex::Import],
    names: &[ImportName],
) -> Result<ImportPreflight, String> {
    let ids = crate::assign_module_ids(libraries);
    let mut module_of = HashMap::new();
    for (name, module) in libraries.iter().zip(ids) {
        if module_of.insert(name.as_str(), module).is_some() {
            return Err("duplicate import-library name".into());
        }
    }
    let mut registered = HashSet::new();
    for export in crate::all_exports() {
        if !registered.insert((export.module, export.ordinal)) {
            return Err("duplicate registered HLE export key".into());
        }
    }
    let mut labels = HashMap::new();
    for name in names {
        let key = (ModuleId(name.module), name.ordinal);
        if labels.insert(key, name.name).is_some_and(|previous| previous != name.name) {
            return Err("conflicting import labels".into());
        }
    }
    let mut result = ImportPreflight::default();
    for import in imports {
        let module = *module_of.get(import.library.as_str()).ok_or("import references undeclared library")?;
        let missing = MissingImport {
            library: import.library.clone(),
            module,
            ordinal: import.ordinal,
            name: labels.get(&(module, import.ordinal)).copied(),
        };
        if import.thunk_addr.is_some() {
            result.function_imports += 1;
            if registered.contains(&(module, import.ordinal)) {
                result.registered_functions += 1;
            } else {
                result.missing_functions.push(missing);
            }
        } else {
            result.variable_imports += 1;
            if Variables::supports(module, import.ordinal) {
                result.supported_variables += 1;
            } else {
                result.missing_variables.push(missing);
            }
        }
    }
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn missing_variables_are_never_diagnostic_placeholders() {
        let mut report = ImportPreflight::default();
        report.missing_variables.push(MissingImport {
            library: "synthetic".into(),
            module: ModuleId(8),
            ordinal: 1,
            name: None,
        });
        assert!(report.validate(ImportPolicy::LegacyStub).is_ok());
        assert!(report.validate(ImportPolicy::StrictAll).is_err());
        assert!(report.validate(ImportPolicy::CpuDiagnosticTrap).is_err());
    }
    #[test]
    fn diagnostic_function_is_a_trap_not_a_return_value() {
        let mut report = ImportPreflight::default();
        report.missing_functions.push(MissingImport {
            library: "synthetic".into(),
            module: ModuleId(8),
            ordinal: 1,
            name: None,
        });
        assert!(report.validate(ImportPolicy::StrictAll).is_err());
        assert!(report.validate(ImportPolicy::CpuDiagnosticTrap).is_ok());
        let payload =
            std::panic::catch_unwind(|| fail_unsupported(ImportPolicy::StrictAll, ModuleId(8), 1)).unwrap_err();
        assert_eq!(
            *payload.downcast::<UnsupportedImport>().unwrap(),
            UnsupportedImport { module: ModuleId(8), ordinal: 1 }
        );
    }
    #[test]
    #[cfg(target_os = "linux")]
    fn cpu_process_contract() {
        const CHILD: &str = "SKYRT_PUBLIC_IMPORT_POLICY_PROCESS_TEST";
        if let Ok(mode) = std::env::var(CHILD) {
            if mode == "installed" {
                install_diagnostic_failure_handler(|failure| {
                    std::process::exit(if failure == (UnsupportedImport { module: ModuleId(8), ordinal: 1 }) {
                        70
                    } else {
                        71
                    })
                })
                .unwrap();
                // An enclosing arbitrary catch must never see this payload.
                let _ = std::panic::catch_unwind(|| fail_unsupported(ImportPolicy::CpuDiagnosticTrap, ModuleId(8), 1));
                std::process::exit(72);
            } else if mode == "panicking" {
                install_diagnostic_failure_handler(|_| panic!("synthetic diagnostic handler panic")).unwrap();
                let _ = std::panic::catch_unwind(|| fail_unsupported(ImportPolicy::CpuDiagnosticTrap, ModuleId(8), 1));
                std::process::exit(72);
            } else if mode == "unregistered" {
                let _ = std::panic::catch_unwind(|| fail_unsupported(ImportPolicy::CpuDiagnosticTrap, ModuleId(8), 1));
                std::process::exit(72);
            } else {
                assert_eq!(ImportPolicy::default(), ImportPolicy::LegacyStub);
                let report = ImportPreflight {
                    missing_functions: vec![MissingImport {
                        library: "synthetic".into(),
                        module: ModuleId(8),
                        ordinal: 1,
                        name: None,
                    }],
                    ..Default::default()
                };
                assert!(report.validate(ImportPolicy::LegacyStub).is_ok());
                std::process::exit(0);
            }
        }
        for (mode, exit) in [("installed", Some(70)), ("unregistered", None), ("panicking", None), ("legacy", Some(0))]
        {
            let output = std::process::Command::new("/bin/sh")
                .args(["-c", "ulimit -c 0; exec \"$@\"", "public-policy-child"])
                .arg(std::env::current_exe().unwrap())
                .args(["--exact", "import_policy::tests::cpu_process_contract", "--nocapture"])
                .env(CHILD, mode)
                .output()
                .unwrap();
            assert_eq!(output.status.code(), exit, "{mode}");
            if exit.is_none() {
                use std::os::unix::process::ExitStatusExt;
                assert_eq!(output.status.signal(), Some(6));
            }
        }
    }
}

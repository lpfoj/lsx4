use orbis_pkg_util::{PkgExtractor, SilentProgress, open_pkg};
use std::env;
use std::path::Path;
use std::process::ExitCode;

fn main() -> ExitCode {
    let mut args = env::args_os();
    let program = args
        .next()
        .and_then(|value| Path::new(&value).file_name().map(|name| name.to_owned()))
        .unwrap_or_else(|| "lsx4-pkg-extract".into());
    let Some(input) = args.next() else {
        eprintln!(
            "usage: {} <input.pkg> <new-output-directory>",
            program.to_string_lossy()
        );
        return ExitCode::from(2);
    };
    let Some(output) = args.next() else {
        eprintln!(
            "usage: {} <input.pkg> <new-output-directory>",
            program.to_string_lossy()
        );
        return ExitCode::from(2);
    };
    if args.next().is_some() {
        eprintln!("error: unexpected extra arguments");
        return ExitCode::from(2);
    }

    let input = Path::new(&input);
    let output = Path::new(&output);
    if !input.is_file() {
        eprintln!("error: input is not a file: {}", input.display());
        return ExitCode::from(2);
    }
    if output.exists() {
        eprintln!(
            "error: refusing to overwrite existing output: {}",
            output.display()
        );
        return ExitCode::from(2);
    }

    let package = match unsafe { open_pkg(input) } {
        Ok(package) => package,
        Err(error) => {
            eprintln!("error: cannot open PKG: {error}");
            return ExitCode::FAILURE;
        }
    };
    match PkgExtractor::new(&package, SilentProgress, false).extract(output) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("error: cannot extract PKG/PFS: {error}");
            ExitCode::FAILURE
        }
    }
}

use anyhow::{bail, Context, Result};

#[tokio::main]
async fn main() -> Result<()> {
    let mut arguments = std::env::args().skip(1);
    let mode = arguments.next().unwrap_or_else(|| "quick".into());
    if let Some(action) = mode.strip_prefix("worker-") {
        let root = std::path::PathBuf::from(arguments.next().context("worker requires root")?);
        let command = if action == "query" {
            sage::Commands::Query {
                action: sage::QueryAction::Installed,
            }
        } else {
            let package = arguments.next().context("worker requires package")?;
            let channel = arguments.next().context("worker requires channel")?;
            match action {
                "install" => sage::Commands::Install {
                    packages: vec![package],
                    channel: Some(channel),
                    no_save: true,
                },
                "remove" => sage::Commands::Remove {
                    packages: vec![package],
                    channel: Some(channel),
                },
                "upgrade" => sage::Commands::Upgrade {
                    packages: vec![package],
                    channel: Some(channel),
                    sync: false,
                },
                _ => bail!("unknown worker mode {mode}"),
            }
        };
        return sage::execute(sage::Cli {
            verbose: false,
            dry_run: false,
            root,
            command,
        })
        .await;
    }
    let mut seed = 0x5a6e_2026_u64;
    let mut operations = 100_usize;
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--seed" => {
                seed = arguments
                    .next()
                    .context("--seed requires an unsigned integer")?
                    .parse()?;
            }
            "--operations" => {
                operations = arguments
                    .next()
                    .context("--operations requires an integer")?
                    .parse()?;
            }
            unknown => bail!("unknown argument {unknown}"),
        }
    }
    match mode.as_str() {
        "quick" => {
            let steps = sage_tests::run_quick().await?;
            println!("quick: ok ({} recorded steps)", steps.len());
        }
        "random" => {
            println!("seed={seed} operations={operations}");
            match sage_tests::run_random(seed, operations).await {
                Ok(steps) => println!("random: ok ({} recorded steps)", steps.len()),
                Err(error) => {
                    eprintln!("random: failed seed={seed}: {error:#}");
                    return Err(error);
                }
            }
        }
        _ => bail!("mode must be quick or random"),
    }
    Ok(())
}

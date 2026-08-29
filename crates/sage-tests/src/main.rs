use anyhow::{bail, Context, Result};

#[tokio::main]
async fn main() -> Result<()> {
    let mut arguments = std::env::args().skip(1);
    let mode = arguments.next().unwrap_or_else(|| "quick".into());
    if mode.starts_with("worker-") {
        let root = std::path::PathBuf::from(arguments.next().context("worker requires root")?);
        let package = arguments.next().context("worker requires package")?;
        let channel = arguments.next().context("worker requires channel")?;
        let command = match mode.as_str() {
            "worker-install" => sage::Commands::Install {
                packages: vec![package],
                channel: Some(channel),
                no_save: true,
            },
            "worker-remove" => sage::Commands::Remove {
                packages: vec![package],
                channel: Some(channel),
            },
            "worker-upgrade" => sage::Commands::Upgrade {
                packages: vec![package],
                channel: Some(channel),
                sync: false,
            },
            _ => bail!("unknown worker mode {mode}"),
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
    let mut packages = 1_000_usize;
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
            "--packages" => {
                packages = arguments
                    .next()
                    .context("--packages requires an integer")?
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
        "bench" => {
            for (metric, value) in sage_tests::run_bench(packages).await? {
                println!("{metric}={value:.3}");
            }
        }
        "all" => {
            sage_tests::run_quick().await?;
            println!("seed={seed} operations={operations}");
            sage_tests::run_random(seed, operations).await?;
            for (metric, value) in sage_tests::run_bench(packages).await? {
                println!("{metric}={value:.3}");
            }
        }
        _ => bail!("mode must be quick, random, bench, or all"),
    }
    Ok(())
}

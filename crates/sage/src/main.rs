//! Sage command-line entry point.

use clap::Parser;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    sage::run(sage::Cli::parse()).await
}

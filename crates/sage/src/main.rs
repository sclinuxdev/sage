//! Sage command-line entry point.

use clap::Parser;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    sage::execute(sage::Cli::parse()).await
}

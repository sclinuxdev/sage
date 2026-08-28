use anyhow::Result;

#[tokio::main]
async fn main() -> Result<()> {
    sage::run().await
}

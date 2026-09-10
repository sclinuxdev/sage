//! Cryptographic signature creation and verification for repository indices.

use ed25519_dalek::{Signature, Signer, Verifier};
pub use ed25519_dalek::{SigningKey, VerifyingKey};
use sage_core::hex;
use std::fs::File;
use std::path::Path;

use crate::error::RepoError;

/// Signs an index file using the Ed25519 private key.
pub fn sign_file(path: &Path, key: &SigningKey) -> Result<Signature, RepoError> {
    let file = File::open(path)?;
    // SAFETY: the published index is not mutated while the map is alive.
    let bytes = unsafe { sage_core::Mmap::map(&file)? };
    Ok(key.sign(&bytes))
}

/// Asynchronously verifies the Ed25519 signature of an index file against a public key.
pub async fn verify_signature(
    path: &Path,
    key_path: &Path,
    signature: &[u8],
) -> Result<(), RepoError> {
    let key = decode_fixed::<32>(&tokio::fs::read(key_path).await?)?;
    let signature = decode_fixed::<64>(signature)?;
    let path = path.to_path_buf();
    tokio::task::spawn_blocking(move || {
        let file = File::open(path)?;
        // SAFETY: the temporary index is immutable for the lifetime of this map.
        let bytes = unsafe { sage_core::Mmap::map(&file)? };
        let key = VerifyingKey::from_bytes(&key).map_err(|_| RepoError::Signature)?;
        key.verify(&bytes, &Signature::from_bytes(&signature))
            .map_err(|_| RepoError::Signature)
    })
    .await?
}

/// Decodes a fixed-size raw or hexadecimal byte sequence.
pub fn decode_fixed<const N: usize>(bytes: &[u8]) -> Result<[u8; N], RepoError> {
    let decoded = if bytes.len() == N {
        bytes.to_vec()
    } else {
        let text = std::str::from_utf8(bytes)
            .map_err(|_| RepoError::Signature)?
            .trim();
        hex::decode(text).map_err(|_| RepoError::Signature)?
    };
    decoded.try_into().map_err(|_| RepoError::Signature)
}

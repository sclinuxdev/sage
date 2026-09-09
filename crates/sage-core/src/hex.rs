//! Minimal, zero-dependency lowercase hexadecimal encoding and decoding.

use std::fmt;

/// Hexadecimal decoding error.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FromHexError;

impl fmt::Display for FromHexError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "invalid hex string")
    }
}

impl std::error::Error for FromHexError {}

/// Encodes a byte sequence into a lowercase hexadecimal string.
pub fn encode(bytes: impl AsRef<[u8]>) -> String {
    const HEX_CHARS: &[u8; 16] = b"0123456789abcdef";
    let bytes = bytes.as_ref();
    let mut string = String::with_capacity(bytes.len() * 2);
    for &byte in bytes {
        string.push(HEX_CHARS[(byte >> 4) as usize] as char);
        string.push(HEX_CHARS[(byte & 0x0f) as usize] as char);
    }
    string
}

/// Decodes a hexadecimal byte sequence or string into a byte vector.
pub fn decode(hex: impl AsRef<[u8]>) -> Result<Vec<u8>, FromHexError> {
    let hex = hex.as_ref();
    if hex.len() % 2 != 0 {
        return Err(FromHexError);
    }
    let mut bytes = Vec::with_capacity(hex.len() / 2);
    let (chunks, _) = hex.as_chunks::<2>();
    for &[c0, c1] in chunks {
        let high = from_hex_digit(c0).ok_or(FromHexError)?;
        let low = from_hex_digit(c1).ok_or(FromHexError)?;
        bytes.push((high << 4) | low);
    }
    Ok(bytes)
}

fn from_hex_digit(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

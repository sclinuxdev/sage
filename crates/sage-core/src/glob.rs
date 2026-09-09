//! Zero-dependency glob pattern matching on file paths.

use std::fmt;
use std::path::Path;

/// Error returned when parsing an invalid glob pattern.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PatternError(pub String);

impl fmt::Display for PatternError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.0)
    }
}

impl std::error::Error for PatternError {}

/// A compiled glob pattern.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Pattern {
    raw: String,
}

impl Pattern {
    /// Compiles a new glob pattern.
    pub fn new(pattern: impl AsRef<str>) -> Result<Self, PatternError> {
        let raw = pattern.as_ref().to_string();
        let mut in_bracket = false;
        for c in raw.chars() {
            if c == '[' {
                in_bracket = true;
            } else if c == ']' {
                in_bracket = false;
            }
        }
        if in_bracket {
            return Err(PatternError(format!("unclosed [ in pattern '{raw}'")));
        }
        Ok(Self { raw })
    }

    pub fn as_str(&self) -> &str {
        &self.raw
    }

    /// Tests whether this pattern matches a given path.
    pub fn matches_path(&self, path: &Path) -> bool {
        let path_str = path.to_string_lossy();
        glob_match(&self.raw, &path_str)
    }
}

impl fmt::Display for Pattern {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.raw)
    }
}

fn glob_match(pat: &str, text: &str) -> bool {
    let pat = pat.strip_prefix('/').unwrap_or(pat);
    let text = text.strip_prefix('/').unwrap_or(text);
    let pat_chars: Vec<char> = pat.chars().collect();
    let text_chars: Vec<char> = text.chars().collect();
    match_chars(&pat_chars, &text_chars)
}

fn match_chars(pat: &[char], text: &[char]) -> bool {
    let mut p = 0;
    let mut t = 0;

    while p < pat.len() && t < text.len() {
        if pat[p..].starts_with(&['/', '*', '*', '/']) {
            if match_chars(&pat[p + 3..], &text[t..]) {
                return true;
            }
            for next_slash in t..text.len() {
                if text[next_slash] == '/' && match_chars(&pat[p + 4..], &text[next_slash + 1..]) {
                    return true;
                }
            }
            return false;
        } else if pat[p..] == ['/', '*', '*'] {
            return text[t] == '/' && text.len() > t + 1;
        } else if pat[p..].starts_with(&['*', '*', '/']) {
            if match_chars(&pat[p + 3..], &text[t..]) {
                return true;
            }
            for next_slash in t..text.len() {
                if text[next_slash] == '/' && match_chars(&pat[p + 3..], &text[next_slash + 1..]) {
                    return true;
                }
            }
            return false;
        } else if pat[p..] == ['*', '*'] {
            return true;
        } else if pat[p] == '*' {
            let next_slash = text[t..]
                .iter()
                .position(|&c| c == '/')
                .map_or(text.len(), |pos| t + pos);
            for i in t..=next_slash {
                if match_chars(&pat[p + 1..], &text[i..]) {
                    return true;
                }
            }
            return false;
        } else if pat[p] == '?' {
            if text[t] == '/' {
                return false;
            }
            p += 1;
            t += 1;
        } else if pat[p] == '[' {
            let Some(close) = pat[p + 1..].iter().position(|&c| c == ']') else {
                return false;
            };
            let content = &pat[p + 1..p + 1 + close];
            let ch = text[t];
            if ch == '/' {
                return false;
            }
            if match_bracket_class(content, ch) {
                p = p + 1 + close + 1;
                t += 1;
            } else {
                return false;
            }
        } else {
            if pat[p] != text[t] {
                return false;
            }
            p += 1;
            t += 1;
        }
    }

    if p == pat.len() && t == text.len() {
        return true;
    }
    if pat[p..] == ['/', '*', '*'] && t < text.len() && t > 0 && text[t - 1] == '/' {
        return true;
    }
    if pat[p..] == ['*', '*'] {
        return true;
    }
    if pat[p..] == ['*'] && t == text.len() {
        return true;
    }
    false
}

fn match_bracket_class(content: &[char], ch: char) -> bool {
    let (negate, chars) = if content.starts_with(&['!']) || content.starts_with(&['^']) {
        (true, &content[1..])
    } else {
        (false, content)
    };
    let mut i = 0;
    let mut matched = false;
    while i < chars.len() {
        if i + 2 < chars.len() && chars[i + 1] == '-' {
            if ch >= chars[i] && ch <= chars[i + 2] {
                matched = true;
                break;
            }
            i += 3;
        } else {
            if chars[i] == ch {
                matched = true;
                break;
            }
            i += 1;
        }
    }
    if negate { !matched } else { matched }
}

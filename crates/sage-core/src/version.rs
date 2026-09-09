//! Version representation, segment parsing, and comparison state machine.

use crate::error::CoreError;
use serde::{Deserialize, Serialize};
use std::cmp::Ordering;
use std::fmt;
use std::str::FromStr;

/// Package version ordered by epoch, upstream version, then package release.
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct Version {
    #[serde(default)]
    pub epoch: u32,
    pub upstream: String,
    pub release: u32,
}

impl Version {
    pub fn new(epoch: u32, upstream: impl Into<String>, release: u32) -> Self {
        Self {
            epoch,
            upstream: upstream.into(),
            release,
        }
    }
}

impl FromStr for Version {
    type Err = CoreError;

    fn from_str(input: &str) -> Result<Self, Self::Err> {
        let (epoch, rest) = match input.split_once(':') {
            Some((value, rest)) => (parse_number(value, input)?, rest),
            None => (0, input),
        };
        let (upstream, release) = rest
            .rsplit_once('-')
            .ok_or_else(|| CoreError::InvalidVersion(input.into()))?;
        if upstream.is_empty() {
            return Err(CoreError::InvalidVersion(input.into()));
        }
        Ok(Self::new(epoch, upstream, parse_number(release, input)?))
    }
}

fn parse_number(value: &str, whole: &str) -> Result<u32, CoreError> {
    value
        .parse()
        .map_err(|_| CoreError::InvalidVersion(whole.into()))
}

impl fmt::Display for Version {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.epoch {
            0 => write!(f, "{}-{}", self.upstream, self.release),
            epoch => write!(f, "{epoch}:{}-{}", self.upstream, self.release),
        }
    }
}

impl PartialOrd for Version {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for Version {
    fn cmp(&self, other: &Self) -> Ordering {
        self.epoch
            .cmp(&other.epoch)
            .then_with(|| compare_upstream(&self.upstream, &other.upstream))
            .then_with(|| self.release.cmp(&other.release))
    }
}

/// Compares alternating numeric and alphabetic runs without integer conversion.
/// Numeric runs ignore leading zeroes and compare by significant length first,
/// so even adversarially long version components cannot overflow.
fn compare_upstream(left: &str, right: &str) -> Ordering {
    let (mut a, mut b) = (left.as_bytes(), right.as_bytes());
    loop {
        a = trim_separators(a);
        b = trim_separators(b);
        if a.is_empty() || b.is_empty() {
            return a.len().cmp(&b.len());
        }
        let numeric = a[0].is_ascii_digit() && b[0].is_ascii_digit();
        if a[0].is_ascii_digit() != b[0].is_ascii_digit() {
            return a[0].is_ascii_digit().cmp(&b[0].is_ascii_digit());
        }
        let (arun, arest) = take_run(a, numeric);
        let (brun, brest) = take_run(b, numeric);
        let order = if numeric {
            compare_numeric(arun, brun)
        } else {
            arun.cmp(brun)
        };
        if order != Ordering::Equal {
            return order;
        }
        (a, b) = (arest, brest);
    }
}

fn trim_separators(mut value: &[u8]) -> &[u8] {
    while value
        .first()
        .is_some_and(|byte| !byte.is_ascii_alphanumeric())
    {
        value = &value[1..];
    }
    value
}

fn take_run(value: &[u8], numeric: bool) -> (&[u8], &[u8]) {
    let end = value
        .iter()
        .position(|byte| byte.is_ascii_digit() != numeric || !byte.is_ascii_alphanumeric())
        .unwrap_or(value.len());
    value.split_at(end)
}

fn compare_numeric(left: &[u8], right: &[u8]) -> Ordering {
    let significant_left = &left[left.iter().take_while(|byte| **byte == b'0').count()..];
    let significant_right = &right[right.iter().take_while(|byte| **byte == b'0').count()..];
    significant_left
        .len()
        .cmp(&significant_right.len())
        .then_with(|| significant_left.cmp(significant_right))
        // Preserve the Ord/Eq contract for differently spelled equal numbers.
        .then_with(|| left.cmp(right))
}

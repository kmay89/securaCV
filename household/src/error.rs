//! One error type for the crate.
//!
//! Every message is a `&'static str` on purpose: an error can never carry a
//! handle, a credential id, a key or any other request-derived text, so an
//! error that reaches a log line or a CLI message cannot leak who presented
//! what. Callers that face the network ([`crate::server`]) never send even
//! these strings; every rejection there is the same `401` body.

use std::fmt;

/// The crate's error.
#[derive(Debug)]
pub enum Error {
    /// The input was malformed or violated a protocol rule.
    Invalid(&'static str),
    /// A presented credential, approval or statement was refused.
    Rejected(&'static str),
    /// The OS random number generator failed. Everything fails closed.
    Rng,
    /// A filesystem or socket operation failed. The message names the
    /// operation, never a path fragment that came from a request.
    Io(&'static str, std::io::Error),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Invalid(m) => write!(f, "invalid: {m}"),
            Error::Rejected(m) => write!(f, "rejected: {m}"),
            Error::Rng => write!(f, "the OS random number generator failed"),
            Error::Io(m, e) => write!(f, "{m}: {e}"),
        }
    }
}

impl std::error::Error for Error {}

/// The crate's result alias.
pub type Result<T> = std::result::Result<T, Error>;

impl Error {
    /// Wrap an I/O error with a static description of the operation.
    pub fn io(what: &'static str) -> impl FnOnce(std::io::Error) -> Error {
        move |e| Error::Io(what, e)
    }
}

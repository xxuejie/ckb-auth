use anyhow::{anyhow, Error};

pub(crate) fn decode_string(s: &str, encoding: &str) -> Result<Vec<u8>, Error> {
    match encoding {
        "hex" => Ok(hex::decode(s)?),
        "base64" => {
            use base64::{engine::general_purpose, Engine as _};
            Ok(general_purpose::STANDARD.decode(s)?)
        }
        "base58" => Ok(bs58::decode(s).into_vec()?),
        "base58_monero" => {
            let b = base58_monero::decode(s)?;
            Ok(b)
        }
        _ => Err(anyhow!("Unknown encoding {}", encoding)),
    }
}

pub(crate) fn encode_to_string<T: AsRef<[u8]>>(s: T, encoding: &str) -> Result<String, Error> {
    match encoding {
        "hex" => Ok(hex::encode(s)),
        "base64" => {
            use base64::{engine::general_purpose, Engine as _};
            Ok(general_purpose::STANDARD.encode(s))
        }
        "base58" => Ok(bs58::encode(s).into_string()),
        _ => Err(anyhow!("Unknown encoding {}", encoding)),
    }
}

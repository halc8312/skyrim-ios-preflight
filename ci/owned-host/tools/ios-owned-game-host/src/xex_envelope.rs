//! Allocation preflight for the already hash-bound plaintext XEX buffer. This
//! is deliberately narrower than the full upstream parser: only raw, completely
//! backed images are staged here; no encrypted/basic/LZX expansion occurs.
use crate::Check;
fn word(d: &[u8], p: usize) -> Check<u32> {
    Ok(u32::from_be_bytes(
        d.get(p..p.checked_add(4).ok_or("XEX header overflow")?)
            .ok_or("XEX header truncated")?
            .try_into()
            .unwrap(),
    ))
}
pub fn validate(data: &[u8], expected_image_size: u32) -> Check<()> {
    if data.get(..4) != Some(b"XEX2")
        || data.len() > ios_signed_image_policy::MAX_IMAGE
        || expected_image_size == 0
        || expected_image_size as usize > ios_signed_image_policy::MAX_IMAGE
    {
        return Err("XEX envelope size or magic");
    }
    let header = word(data, 8)? as usize;
    let security = word(data, 16)? as usize;
    let count = word(data, 20)? as usize;
    if header < 24
        || header > data.len()
        || count > 4096
        || count > (header - 24) / 8
        || security.checked_add(0x184).is_none_or(|n| n > header)
    {
        return Err("XEX envelope header/table bounds");
    }
    if word(data, security + 4)? != expected_image_size
        || data.len() - header != expected_image_size as usize
    {
        return Err("XEX envelope image size or backing mismatch");
    }
    let mut keys = std::collections::BTreeSet::new();
    let mut plain_format = false;
    for i in 0..count {
        let at = 24 + i * 8;
        let key = word(data, at)?;
        if !keys.insert(key) {
            return Err("XEX duplicate optional header");
        }
        if key == 0x3ff {
            plain_format = true;
            let p = word(data, at + 4)? as usize;
            if p.checked_add(8).is_none_or(|end| end > header)
                || word(data, p)? < 8
                || word(data, p)? as usize > header - p
                || data[p + 4..p + 8] != [0, 0, 0, 0]
            {
                return Err("XEX requires explicitly plaintext uncompressed format");
            }
        }
    }
    if !plain_format {
        return Err("XEX explicit plaintext format missing");
    }
    Ok(())
}
#[cfg(test)]
mod tests {
    use super::*;
    fn put(d: &mut [u8], p: usize, v: u32) {
        d[p..p + 4].copy_from_slice(&v.to_be_bytes());
    }
    fn fixture() -> Vec<u8> {
        let mut d = vec![0; 0x1200];
        d[..4].copy_from_slice(b"XEX2");
        for (p, v) in [
            (8, 0x200),
            (16, 0x40),
            (20, 1),
            (24, 0x3ff),
            (28, 0x1c8),
            (0x44, 0x1000),
            (0x1c8, 8),
        ] {
            put(&mut d, p, v);
        }
        d
    }
    #[test]
    fn bounded_complete_plaintext_before_full_parse() {
        let d = fixture();
        validate(&d, 0x1000).unwrap();
        for end in 0..0x200 {
            assert!(validate(&d[..end], 0x1000).is_err());
        }
        assert!(validate(&d[..d.len() - 1], 0x1000).is_err());
        let mut extra = d.clone();
        extra.push(0);
        assert!(validate(&extra, 0x1000).is_err());
        assert!(validate(&d, 0x2000).is_err());
        let mut changed = d.clone();
        put(&mut changed, 20, u32::MAX);
        assert!(validate(&changed, 0x1000).is_err());
        let mut changed = d;
        put(&mut changed, 0x44, u32::MAX);
        assert!(validate(&changed, 0x1000).is_err());
    }
    #[test]
    fn compressed_encrypted_or_aliasing_header_records_refuse() {
        let mut missing = fixture();
        put(&mut missing, 20, 0);
        assert!(validate(&missing, 0x1000).is_err());
        for p in [0x1cc, 0x1ce, 0x1cf] {
            let mut d = fixture();
            d[p] = 1;
            assert!(validate(&d, 0x1000).is_err());
        }
        let mut d = fixture();
        put(&mut d, 20, 2);
        put(&mut d, 32, 0x3ff);
        put(&mut d, 36, 0x1c8);
        assert!(validate(&d, 0x1000).is_err());
        let mut d = fixture();
        put(&mut d, 28, 0x1ff);
        assert!(validate(&d, 0x1000).is_err());
    }
}

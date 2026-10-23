//! Authored display overrides. Preserve all unrelated bytes, including legacy
//! encodings. This returns a separate file; the caller never edits the disc.
pub const MAX_INI_BYTES: u64 = 65_536;
pub fn video_override(input: &[u8], width: u32, height: u32) -> Result<Vec<u8>, &'static str> {
    if input.len() as u64 > MAX_INI_BYTES || input.contains(&0) || input.starts_with(&[0xef, 0xbb, 0xbf])
        || input.windows(2).any(|pair| pair[0] == b'\r' && pair[1] != b'\n') || input.ends_with(b"\r")
        || !crate::device_settings::VIDEO_MODES.contains(&(width, height)) {
        return Err("unsupported display configuration input");
    }
    let values = format!("iSize W={width}\r\niSize H={height}\r\n");
    let mut output = Vec::with_capacity(input.len() + 128);
    let mut display = false;
    let mut found = false;
    for line in input.split_inclusive(|byte| *byte == b'\n') {
        let trimmed = line.trim_ascii();
        if trimmed.starts_with(b"[") {
            let end = trimmed.iter().position(|byte| *byte == b']').ok_or("ambiguous INI section header")?;
            let trailing = trimmed[end+1..].trim_ascii();
            if !trailing.is_empty() && !trailing.starts_with(b";") && !trailing.starts_with(b"#") {
                return Err("ambiguous INI section suffix");
            }
            display = trimmed[1..end].trim_ascii().eq_ignore_ascii_case(b"Display");
            output.extend_from_slice(line);
            if display {
                found = true;
                if !line.ends_with(b"\n") { output.extend_from_slice(b"\r\n"); }
                output.extend_from_slice(values.as_bytes());
            }
            continue;
        }
        if display && let Some(equal) = trimmed.iter().position(|byte| *byte == b'=') {
            let key = trimmed[..equal].trim_ascii();
            if key.eq_ignore_ascii_case(b"iSize W") || key.eq_ignore_ascii_case(b"iSize H") { continue; }
        }
        output.extend_from_slice(line);
    }
    if !found {
        if !output.is_empty() && !output.ends_with(b"\n") { output.extend_from_slice(b"\r\n"); }
        output.extend_from_slice(b"[Display]\r\n");
        output.extend_from_slice(values.as_bytes());
    }
    if output.len() as u64 > MAX_INI_BYTES + 1024 { return Err("display configuration exceeds bound"); }
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn legacy_language_archive_and_unrelated_sections_remain_exact() {
        let original = b"[General]\r\nsLanguage=\x93\xfa\x96\x7b\r\n[Archive]\r\nList=owned.bsa\r\n";
        let output = video_override(original, 640, 360).unwrap();
        assert!(output.starts_with(original));
        assert!(output.ends_with(b"[Display]\r\niSize W=640\r\niSize H=360\r\n"));
    }
    #[test]
    fn every_display_section_gets_same_tuple_and_old_values_are_removed() {
        let input = b"[Display]\niSize W=1280\n iSIZE h = 720\nOther=7\n[Other]\niSize W=9\n[ display ]\niSize H=10";
        let out = video_override(input, 960, 540).unwrap();
        let text = std::str::from_utf8(&out).unwrap();
        assert_eq!(text.matches("iSize W=960").count(), 2);
        assert_eq!(text.matches("iSize H=540").count(), 2);
        assert!(text.contains("Other=7\n[Other]\niSize W=9\n"));
        assert!(!text.contains("1280") && !text.contains("720") && !text.contains("H=10"));
    }
    #[test]
    fn missing_final_newline_and_repeated_override_are_stable() {
        for input in [b"".as_slice(), b"[Display]", b"[Other]\nValue=1"] {
            let once = video_override(input, 1920, 1080).unwrap();
            assert_eq!(video_override(&once, 1920, 1080).unwrap(), once);
        }
    }
    #[test]
    fn binary_or_unbounded_input_and_unknown_sizes_are_refused() {
        assert!(video_override(b"a\0b", 640, 360).is_err());
        assert!(video_override(&vec![b'a'; MAX_INI_BYTES as usize + 1], 640, 360).is_err());
        assert!(video_override(b"", 1, 1).is_err());
    }
    #[test]
    fn commented_headers_cannot_remove_another_sections_values() {
        let input = b"[Display] ; first\niSize W=1280\n[Other] # keep\niSize W=9\n[Display]#again\niSize H=720\n";
        let output = video_override(input, 640, 360).unwrap();
        let text = std::str::from_utf8(&output).unwrap();
        assert!(text.contains("[Other] # keep\niSize W=9\n"));
        assert_eq!(text.matches("iSize W=640").count(), 2);
        assert_eq!(text.matches("iSize H=360").count(), 2);
    }
    #[test]
    fn unsupported_encoding_line_endings_and_ambiguous_headers_are_refused() {
        for input in [b"\xef\xbb\xbf[Display]\n".as_slice(), b"[Display]\riSize W=5\r", b"[Display]oops\n", b"[Display\n"] {
            assert!(video_override(input, 640, 360).is_err());
        }
    }
}

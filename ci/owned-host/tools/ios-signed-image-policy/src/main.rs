use ios_signed_image_policy::{
    Check, MAX_IMAGE, candidate_make_manifest, candidate_verify_binding, load_manifest,
    make_manifest, signature_shape, verify_binding,
};
use serde_json::{Value, json};
use std::{
    fs::File,
    io::{Read, Write},
    path::Path,
};
fn read(path: &Path, limit: usize) -> Check<Vec<u8>> {
    let mut bytes = Vec::new();
    File::open(path)
        .map_err(|_| "input_read")?
        .take((limit + 1) as u64)
        .read_to_end(&mut bytes)
        .map_err(|_| "input_read")?;
    if bytes.len() > limit {
        Err("input_limit")
    } else {
        Ok(bytes)
    }
}
fn unhex(text: &str) -> Check<Vec<u8>> {
    if text.len() % 2 != 0 || text.len() > 32 * 1024 * 1024 {
        return Err("public_vector_size");
    }
    let mut bytes = Vec::with_capacity(text.len() / 2);
    for pair in text.as_bytes().chunks_exact(2) {
        let value = std::str::from_utf8(pair).map_err(|_| "public_vector_hex")?;
        bytes.push(u8::from_str_radix(value, 16).map_err(|_| "public_vector_hex")?);
    }
    Ok(bytes)
}
fn vector(case: &Value) -> Check<Value> {
    let op = case
        .get("operation")
        .and_then(Value::as_str)
        .ok_or("vector_schema")?;
    let data = unhex(
        case.get("data_hex")
            .and_then(Value::as_str)
            .ok_or("vector_schema")?,
    )?;
    match op {
        "manifest" => Ok(make_manifest(&data)?.to_json()),
        "signature" => {
            signature_shape(&data)?;
            Ok(Value::Null)
        }
        // Python's loader checks duplicate keys/size, while its strict schema
        // is validated at verify. This Rust loader intentionally combines both.
        "load_manifest" => Ok(load_manifest(&data)?.to_json()),
        "verify" => {
            let text = case
                .get("manifest_json")
                .and_then(Value::as_str)
                .ok_or("vector_schema")?;
            let expected = case
                .get("expected_sha256")
                .and_then(Value::as_str)
                .ok_or("vector_schema")?;
            Ok(verify_binding(&data, &load_manifest(text.as_bytes())?, expected)?.to_json())
        }
        _ => Err("vector_operation"),
    }
}
fn execute(args: Vec<std::ffi::OsString>) -> Check<Value> {
    match args.get(1).and_then(|v| v.to_str()) {
        Some(mode @ ("manifest" | "candidate-manifest")) if args.len() == 4 => {
            let candidate = mode == "candidate-manifest";
            let data = read(Path::new(&args[2]), MAX_IMAGE)?;
            let m = if candidate {
                candidate_make_manifest(&data)?
            } else {
                make_manifest(&data)?
            };
            let mut out = File::options()
                .write(true)
                .create_new(true)
                .open(&args[3])
                .map_err(|_| "new_manifest_destination")?;
            let mut bytes = serde_json::to_vec_pretty(&m.to_json()).map_err(|_| "json_output")?;
            bytes.push(b'\n');
            out.write_all(&bytes).map_err(|_| "manifest_write")?;
            let mut value=json!({"manifest_created":true,"unsigned_sha256":m.unsigned_sha256});
            if candidate{value["candidate_policy"]=Value::Bool(true);}
            Ok(value)
        }
        Some(mode @ ("verify" | "candidate-verify"))
            if args.len() == 6 && args[4] == "--expected-unsigned-sha256" =>
        {
            let m = load_manifest(&read(Path::new(&args[3]), 16384)?)?;
            let expected = args[5].to_str().ok_or("trusted_unsigned_sha256")?;
            let data = read(Path::new(&args[2]), MAX_IMAGE)?;
            Ok(if mode == "candidate-verify" {
                candidate_verify_binding(&data, &m, expected)?
            } else {
                verify_binding(&data, &m, expected)?
            }
            .to_json())
        }
        Some("public-vectors") if args.len() == 3 => {
            let data = read(Path::new(&args[2]), 128 * 1024 * 1024)?;
            let values: Value = serde_json::from_slice(&data).map_err(|_| "public_vectors_json")?;
            let cases = values.as_array().ok_or("public_vectors_schema")?;
            let mut mismatches = Vec::new();
            for (index, case) in cases.iter().enumerate() {
                let result = std::panic::catch_unwind(|| vector(case));
                let expected = case
                    .get("accepted")
                    .and_then(Value::as_bool)
                    .ok_or("public_vectors_schema")?;
                let same = match result {
                    Ok(Ok(actual)) => expected && case.get("expected_result") == Some(&actual),
                    Ok(Err(_)) => !expected,
                    Err(_) => false,
                };
                if !same {
                    mismatches.push(index);
                }
            }
            Ok(
                json!({"public_vector_count":cases.len(),"mismatch_count":mismatches.len(),"mismatch_indices":mismatches,"game_executed":false,"loader_integration_allowed":false}),
            )
        }
        _ => Err("arguments"),
    }
}
fn main() {
    let value = execute(std::env::args_os().collect());
    let success = value.is_ok();
    let output=value.map(|v|json!({"success":true,"result":v})).unwrap_or_else(|_|json!({"success":false,"category":"binding_rejected","os_signature_authenticated":false}));
    println!("{output}");
    if !success {
        std::process::exit(2)
    }
}

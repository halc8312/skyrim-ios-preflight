
@group(0) @binding(0) var<storage, read> records: array<u32>;
@group(0) @binding(1) var<storage, read> values: array<vec4<u32>>;
@group(0) @binding(2) var<storage, read_write> blocks: array<vec4<u32>>;

@compute @workgroup_size(256)
fn main(@builtin(workgroup_id) group: vec3<u32>, @builtin(local_invocation_index) t: u32) {
    let header = 1u + group.x * 3u;
    if (t >= records[header + 2u]) {
        return;
    }
    let end = records[header] + records[header + 1u];
    let word_index = t / 32u;
    let bit = 1u << (t % 32u);
    var current = vec4<u32>(0u);
    for (var index = records[header]; index < end; index++) {
        let b = 16u + records[index] * 12u;
        let mask = records[b + 4u + word_index];
        if ((mask & bit) != 0u) {
            var n = countOneBits(mask & (bit - 1u));
            for (var w = 0u; w < word_index; w++) {
                n += countOneBits(records[b + 4u + w]);
            }
            current = values[records[b + 3u] + n];
        }
        if (t < records[b + 2u]) {
            blocks[records[b + 1u] + t] = current;
        }
    }
}

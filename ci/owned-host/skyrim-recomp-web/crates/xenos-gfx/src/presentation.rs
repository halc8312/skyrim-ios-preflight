//! The established presentation shaders and aspect transform, shared by both backends.
pub const BLIT: &str = r#"
struct VsOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
}

@vertex
fn vs(@builtin(vertex_index) i: u32) -> VsOut {
    let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
    var o: VsOut;
    o.pos = vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = vec2<f32>(p.x, 1.0 - p.y);
    return o;
}

@group(0) @binding(0) var frame: texture_2d<f32>;
@group(0) @binding(1) var frame_sampler: sampler;
// Per output channel: host channel 0-3, 4 = zero, 5 = one (the fetch constant's swizzle).
@group(0) @binding(2) var<uniform> swizzle: vec4<u32>;

fn pick(t: vec4<f32>, s: u32) -> f32 {
    switch s {
        case 0u: { return t.x; }
        case 1u: { return t.y; }
        case 2u: { return t.z; }
        case 3u: { return t.w; }
        case 4u: { return 0.0; }
        default: { return 1.0; }
    }
}

// Catmull-Rom (bicubic) filtering in 9 bilinear taps (the 16 texels' weights folded pairwise):
// sharper than bilinear when the frame is scaled up to the screen.
fn catmull_rom(uv: vec2<f32>) -> vec4<f32> {
    let size = vec2<f32>(textureDimensions(frame));
    let pos = uv * size;
    let center = floor(pos - 0.5) + 0.5;
    let f = pos - center;
    let w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    let w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    let w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    let w3 = f * f * (-0.5 + 0.5 * f);
    let w12 = w1 + w2;
    let t0 = (center - 1.0) / size;
    let t3 = (center + 2.0) / size;
    let t12 = (center + w2 / w12) / size;
    var c = vec4<f32>(0.0);
    c += textureSampleLevel(frame, frame_sampler, vec2<f32>(t0.x, t0.y), 0.0) * w0.x * w0.y;
    c += textureSampleLevel(frame, frame_sampler, vec2<f32>(t12.x, t0.y), 0.0) * w12.x * w0.y;
    c += textureSampleLevel(frame, frame_sampler, vec2<f32>(t3.x, t0.y), 0.0) * w3.x * w0.y;
    c += textureSampleLevel(frame, frame_sampler, vec2<f32>(t0.x, t12.y), 0.0) * w0.x * w12.y;
    c += textureSampleLevel(frame, frame_sampler, vec2<f32>(t12.x, t12.y), 0.0) * w12.x * w12.y;
    c += textureSampleLevel(frame, frame_sampler, vec2<f32>(t3.x, t12.y), 0.0) * w3.x * w12.y;
    c += textureSampleLevel(frame, frame_sampler, vec2<f32>(t0.x, t3.y), 0.0) * w0.x * w3.y;
    c += textureSampleLevel(frame, frame_sampler, vec2<f32>(t12.x, t3.y), 0.0) * w12.x * w3.y;
    c += textureSampleLevel(frame, frame_sampler, vec2<f32>(t3.x, t3.y), 0.0) * w3.x * w3.y;
    return max(c, vec4<f32>(0.0));
}

@fragment
fn fs(in: VsOut) -> @location(0) vec4<f32> {
    // Scaled up (the viewport is larger than the frame): bicubic; otherwise bilinear.
    let scale = fwidth(in.uv) * vec2<f32>(textureDimensions(frame));
    var t: vec4<f32>;
    if (max(scale.x, scale.y) < 0.99) {
        t = catmull_rom(in.uv);
    } else {
        t = textureSampleLevel(frame, frame_sampler, in.uv, 0.0);
    }
    return vec4<f32>(pick(t, swizzle.x), pick(t, swizzle.y), pick(t, swizzle.z), 1.0);
}
"#;

pub fn filtered_shader() -> String {
    let helpers = BLIT.split_once("@fragment\n").expect("one original fragment entry").0;
    format!(
        "{helpers}{}",
        r#"
@group(0) @binding(3) var<uniform> options: vec4<u32>;
@fragment
fn fs(in: VsOut) -> @location(0) vec4<f32> {
    let dimensions = textureDimensions(frame);
    let size = vec2<f32>(dimensions);
    let scale = fwidth(in.uv) * size;
    var t: vec4<f32>;
    if (options.x == 2u) {
        let p = clamp(vec2<i32>(floor(in.uv * size)), vec2<i32>(0), vec2<i32>(dimensions) - 1);
        t = textureLoad(frame, p, 0);
    } else if (options.x == 3u || (options.x == 0u && max(scale.x, scale.y) < 0.99)) {
        t = catmull_rom(in.uv);
    } else {
        t = textureSampleLevel(frame, frame_sampler, in.uv, 0.0);
    }
    if (options.y > 0u) {
        let step = 1.0 / size;
        let neighbors = textureSampleLevel(frame, frame_sampler, in.uv + vec2<f32>(step.x, 0.0), 0.0)
            + textureSampleLevel(frame, frame_sampler, in.uv - vec2<f32>(step.x, 0.0), 0.0)
            + textureSampleLevel(frame, frame_sampler, in.uv + vec2<f32>(0.0, step.y), 0.0)
            + textureSampleLevel(frame, frame_sampler, in.uv - vec2<f32>(0.0, step.y), 0.0);
        let amount = f32(options.y) * 0.005;
        t = clamp(t + (4.0 * t - neighbors) * amount, vec4<f32>(0.0), vec4<f32>(1.0));
    }
    return vec4<f32>(pick(t, swizzle.x), pick(t, swizzle.y), pick(t, swizzle.z), 1.0);
}
"#
    )
}

pub fn letterbox(frame: (u32, u32), target: (u32, u32)) -> (f32, f32, f32, f32) {
    let (fw, fh) = (frame.0.max(1) as f32, frame.1.max(1) as f32);
    let (tw, th) = (target.0 as f32, target.1 as f32);
    let scale = (tw / fw).min(th / fh);
    let (w, h) = (fw * scale, fh * scale);
    ((tw - w) / 2.0, (th - h) / 2.0, w, h)
}

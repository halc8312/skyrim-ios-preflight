//! Shader interface variants preserve the complete original body as a helper.
//! This removes only unbound color outputs, never side effects or depth/coverage.
use super::*;
use naga::{Expression as E, Statement as S, TypeInner};

/// Move the original entry body into an ordinary helper, retaining structured
/// control flow and discard. The wrapper calls it once with the original inputs.
pub(super) fn wrapper(
    module: &mut naga::Module,
    index: usize,
) -> (
    naga::Function,
    Option<naga::FunctionResult>,
    Option<naga::Handle<E>>,
) {
    let mut original = std::mem::take(&mut module.entry_points[index].function);
    let mut wrapper = naga::Function {
        arguments: original.arguments.clone(),
        ..Default::default()
    };
    let result = original.result.clone();
    for arg in &mut original.arguments {
        arg.binding = None;
    }
    if let Some(result) = &mut original.result {
        result.binding = None;
    }
    original.name = Some(format!(
        "native_interface_source_{}",
        module.entry_points[index].name
    ));
    let function = module.functions.append(original, naga::Span::UNDEFINED);
    let arguments = (0..wrapper.arguments.len())
        .map(|n| {
            wrapper
                .expressions
                .append(E::FunctionArgument(n as u32), naga::Span::UNDEFINED)
        })
        .collect();
    let returned = result.as_ref().map(|_| {
        wrapper
            .expressions
            .append(E::CallResult(function), naga::Span::UNDEFINED)
    });
    wrapper.body.push(
        S::Call {
            function,
            arguments,
            result: returned,
        },
        naga::Span::UNDEFINED,
    );
    (wrapper, result, returned)
}

impl<B: Backend> Gpu<B> {
    /// Retain color locations selected by `locations` and every built-in output.
    /// This changes no shader invocation/side-effect semantics and preserves the
    /// input module. Exact no-op variants reuse the source's immutable identity.
    pub fn shader_with_color_targets(
        &self,
        source: &Shader<B>,
        entry: &str,
        locations: u32,
    ) -> Result<Shader<B>> {
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &source.0.owner) {
            return Err(Error::Rejected("color interface shader device"));
        }
        let index = source.entry(entry, naga::ShaderStage::Fragment)?;
        let existing = super::outputs(&source.0.module, &source.0.module.entry_points[index])?;
        if existing
            .locations
            .keys()
            .all(|&n| n < 32 && locations & (1 << n) != 0)
        {
            return Ok(source.clone());
        }
        let mut module = source.0.module.clone();
        prune(&mut module, index, locations)?;
        let info = wgpu_naga_bridge::create_validator(
            self.owner.features,
            self.owner.downlevel,
            naga::valid::ValidationFlags::all(),
        )
        .validate(&module)
        .map_err(backend_error)?;
        self.shader_module(module, info)
    }
}
fn prune(module: &mut naga::Module, index: usize, locations: u32) -> Result<()> {
    let (mut out, original, returned) = wrapper(module, index);
    let result = original.ok_or(Error::Rejected("color interface has no output"))?;
    let keep = |binding: &Option<naga::Binding>| match binding {
        Some(naga::Binding::Location { location, .. }) => {
            *location < 32 && locations & (1 << location) != 0
        }
        _ => true,
    };
    let start = out.expressions.len();
    let value = if result.binding.is_some() {
        if keep(&result.binding) {
            out.result = Some(result);
            returned
        } else {
            None
        }
    } else {
        let TypeInner::Struct { members, span } = &module.types[result.ty].inner else {
            return Err(Error::Rejected("color output interface"));
        };
        let span = *span;
        let mut retained = Vec::new();
        let mut components = Vec::new();
        for (n, member) in members.iter().enumerate() {
            if keep(&member.binding) {
                retained.push(member.clone());
                components.push(out.expressions.append(
                    E::AccessIndex {
                        base: returned.unwrap(),
                        index: n as u32,
                    },
                    naga::Span::UNDEFINED,
                ));
            }
        }
        if retained.is_empty() {
            None
        } else {
            let ty = module.types.insert(
                naga::Type {
                    name: Some("NativeColorInterface".into()),
                    inner: TypeInner::Struct {
                        members: retained,
                        span,
                    },
                },
                naga::Span::UNDEFINED,
            );
            out.result = Some(naga::FunctionResult { ty, binding: None });
            Some(
                out.expressions
                    .append(E::Compose { ty, components }, naga::Span::UNDEFINED),
            )
        }
    };
    if out.expressions.len() > start {
        out.body.push(
            S::Emit(out.expressions.range_from(start)),
            naga::Span::UNDEFINED,
        );
    }
    out.body.push(S::Return { value }, naga::Span::UNDEFINED);
    module.entry_points[index].function = out;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn pruning_keeps_external_writes_in_the_validated_ir_even_with_no_color_result() {
        let source = "@group(0) @binding(0) var<storage,read_write> values:array<u32>; @fragment fn fs()->@location(0) vec4f{values[0]=123u;return vec4f(1.);}";
        let mut module = naga::front::wgsl::parse_str(source).unwrap();
        let validate = |m: &naga::Module| {
            naga::valid::Validator::new(
                naga::valid::ValidationFlags::all(),
                naga::valid::Capabilities::all(),
            )
            .validate(m)
            .unwrap()
        };
        let before = validate(&module);
        let global = module.global_variables.iter().next().unwrap().0;
        assert!(before.get_entry_point(0)[global].contains(naga::valid::GlobalUse::WRITE));
        prune(&mut module, 0, 0).unwrap();
        let after = validate(&module);
        assert!(module.entry_points[0].function.result.is_none());
        assert_eq!(
            before.get_entry_point(0)[global],
            after.get_entry_point(0)[global]
        );
        // Native graphics-storage bindings still reject, independently of this
        // IR preservation proof. No GPU storage-write support is claimed here.
    }
    #[test]
    fn color_pruning_and_coverage_wrappers_emit_real_msl_without_fake_bindings() {
        for source in [
            "@fragment fn fs()->@location(0) vec4f{return vec4f(1.);}",
            "struct O{@location(0) a:vec4f,@location(2) b:vec4f,@builtin(frag_depth)d:f32,@builtin(sample_mask)m:u32} @fragment fn fs()->O{return O(vec4f(1.),vec4f(0.,1.,0.,1.),0.25,3u);}",
        ] {
            for colors in [0, 4] {
                let mut module = naga::front::wgsl::parse_str(source).unwrap();
                let validate = |m: &naga::Module| {
                    naga::valid::Validator::new(
                        naga::valid::ValidationFlags::all(),
                        naga::back::msl::supported_capabilities(),
                    )
                    .validate(m)
                    .unwrap()
                };
                validate(&module);
                prune(&mut module, 0, colors).unwrap();
                let info = validate(&module);
                let output = super::super::outputs(&module, &module.entry_points[0]).unwrap();
                assert!(output.locations.keys().all(|&n| colors & (1 << n) != 0));
                super::super::sample_mask::lower(&mut module, &info, 0, 9).unwrap();
                let info = validate(&module);
                let (msl, translation) = naga::back::msl::write_string(
                    &module,
                    &info,
                    &naga::back::msl::Options {
                        lang_version: (2, 3),
                        fake_missing_bindings: false,
                        ..Default::default()
                    },
                    &naga::back::msl::PipelineOptions {
                        entry_point: Some((naga::ShaderStage::Fragment, "fs".into())),
                        ..Default::default()
                    },
                )
                .unwrap();
                assert!(
                    translation
                        .entry_point_names
                        .iter()
                        .all(|entry| entry.is_ok())
                );
                assert!(msl.contains("[[sample_mask]]"));
                assert!(!msl.contains("[[color(0)]]"));
                if colors == 0 {
                    assert!(!msl.contains("[[color(2)]]"));
                }
            }
        }
    }
}

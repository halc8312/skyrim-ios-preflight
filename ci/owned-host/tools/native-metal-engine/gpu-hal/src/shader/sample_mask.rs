//! A deliberately restricted output-coverage lowering for backends without a
//! fixed sample mask. It is NOT equivalent for arbitrary fragment programs:
//! fixed masks precede fragment shading, output masks follow it. Reject coverage
//! inputs, centroid/sample interpolation, forced early tests and externally
//! observable writes rather than silently changing their meaning.
use super::*;
use naga::{Binding, BuiltIn, Expression as E, FunctionResult, Statement as S, TypeInner};

impl<B: Backend> Gpu<B> {
    /// Create an immutable variant, preserving the source shader and every
    /// other entry point. Callers must disable hardware alpha-to-coverage; the
    /// normal pipeline validation rejects combining it with this mask output.
    /// The restricted transformation is exposed on Vulkan too for comparison
    /// against the independent fixed-function sample-mask implementation.
    pub fn shader_with_sample_mask(
        &self,
        source: &Shader<B>,
        entry: &str,
        mask: u32,
    ) -> Result<Shader<B>> {
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &source.0.owner) {
            return Err(Error::Rejected("sample-mask shader device"));
        }
        let index = source.entry(entry, naga::ShaderStage::Fragment)?;
        let mut module = source.0.module.clone();
        lower(&mut module, &source.0.info, index, mask)?;
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

pub(super) fn lower(
    module: &mut naga::Module,
    info: &naga::valid::ModuleInfo,
    index: usize,
    mask: u32,
) -> Result<()> {
    let entry = &module.entry_points[index];
    let input = inputs(module, entry)?;
    if entry.early_depth_test.is_some()
        || input
            .builtins
            .iter()
            .any(|b| !matches!(b, BuiltIn::Position { .. } | BuiltIn::FrontFacing))
        || input.locations.values().any(|i| {
            matches!(
                i.sampling,
                Some(naga::Sampling::Centroid | naga::Sampling::Sample)
            )
        })
    {
        return Err(Error::Rejected(
            "sample-mask lowering changes fragment invocation inputs",
        ));
    }
    let usage = info.get_entry_point(index);
    for (h, global) in module.global_variables.iter() {
        if global.space != naga::AddressSpace::Private
            && usage[h].intersects(naga::valid::GlobalUse::WRITE | naga::valid::GlobalUse::ATOMIC)
        {
            return Err(Error::Rejected(
                "sample-mask lowering changes fragment side effects",
            ));
        }
    }
    // Wrapping the old function handles all its returns, switches, loops and
    // discard paths without moving expressions across their original scope.
    // Existing helpers precede the appended function, preserving Naga's DAG.
    let (mut wrapper, old_result, returned) = super::color_outputs::wrapper(module, index);
    let constant = wrapper
        .expressions
        .append(E::Literal(naga::Literal::U32(mask)), naga::Span::UNDEFINED);
    let uint = module.types.insert(
        naga::Type {
            name: None,
            inner: TypeInner::Scalar(naga::Scalar::U32),
        },
        naga::Span::UNDEFINED,
    );
    let mask_binding = Some(Binding::BuiltIn(BuiltIn::SampleMask));
    let start = wrapper.expressions.len();
    let output;
    match old_result {
        None => {
            wrapper.result = Some(FunctionResult {
                ty: uint,
                binding: mask_binding,
            });
            output = constant;
        }
        Some(result) if result.binding == mask_binding => {
            wrapper.result = Some(result);
            output = wrapper.expressions.append(
                E::Binary {
                    op: naga::BinaryOperator::And,
                    left: returned.unwrap(),
                    right: constant,
                },
                naga::Span::UNDEFINED,
            );
        }
        Some(result) => {
            let mut layout = naga::proc::Layouter::default();
            layout.update(module.to_ctx()).map_err(backend_error)?;
            let (mut members, old_structure, old_span) = match &module.types[result.ty].inner {
                TypeInner::Struct { members, span } if result.binding.is_none() => {
                    (members.clone(), true, *span)
                }
                _ => (
                    vec![naga::StructMember {
                        name: Some("value".into()),
                        ty: result.ty,
                        binding: result.binding,
                        offset: 0,
                    }],
                    false,
                    layout[result.ty].size,
                ),
            };
            let old_count = members.len();
            let existing = members.iter().position(|m| m.binding == mask_binding);
            let ty = if existing.is_some() {
                result.ty
            } else {
                let offset = naga::proc::Alignment::FOUR.round_up(old_span);
                members.push(naga::StructMember {
                    name: Some("native_coverage".into()),
                    ty: uint,
                    binding: mask_binding,
                    offset,
                });
                let alignment = layout[result.ty].alignment.max(naga::proc::Alignment::FOUR);
                module.types.insert(
                    naga::Type {
                        name: Some("NativeCoverageResult".into()),
                        inner: TypeInner::Struct {
                            members,
                            span: alignment.round_up(offset + 4),
                        },
                    },
                    naga::Span::UNDEFINED,
                )
            };
            let mut components = Vec::with_capacity(old_count + usize::from(existing.is_none()));
            for n in 0..old_count {
                let value = if old_structure {
                    wrapper.expressions.append(
                        E::AccessIndex {
                            base: returned.unwrap(),
                            index: n as u32,
                        },
                        naga::Span::UNDEFINED,
                    )
                } else {
                    returned.unwrap()
                };
                components.push(if existing == Some(n) {
                    wrapper.expressions.append(
                        E::Binary {
                            op: naga::BinaryOperator::And,
                            left: value,
                            right: constant,
                        },
                        naga::Span::UNDEFINED,
                    )
                } else {
                    value
                });
            }
            if existing.is_none() {
                components.push(constant);
            }
            output = wrapper
                .expressions
                .append(E::Compose { ty, components }, naga::Span::UNDEFINED);
            wrapper.result = Some(FunctionResult { ty, binding: None });
        }
    }
    if wrapper.expressions.len() > start {
        wrapper.body.push(
            S::Emit(wrapper.expressions.range_from(start)),
            naga::Span::UNDEFINED,
        );
    }
    wrapper.body.push(
        S::Return {
            value: Some(output),
        },
        naga::Span::UNDEFINED,
    );
    module.entry_points[index].function = wrapper;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn early_test_variants_are_rejected_before_rewriting_the_function() {
        for early in [
            naga::EarlyDepthTest::Force,
            naga::EarlyDepthTest::Allow {
                conservative: naga::ConservativeDepth::Unchanged,
            },
        ] {
            let mut module = naga::front::wgsl::parse_str("@fragment fn fs(){}").unwrap();
            let info = naga::valid::Validator::new(
                naga::valid::ValidationFlags::all(),
                naga::valid::Capabilities::all(),
            )
            .validate(&module)
            .unwrap();
            module.entry_points[0].early_depth_test = Some(early);
            let functions = module.functions.len();
            assert!(lower(&mut module, &info, 0, 9).is_err());
            assert_eq!(module.functions.len(), functions);
            assert!(module.entry_points[0].function.result.is_none());
        }
    }
}

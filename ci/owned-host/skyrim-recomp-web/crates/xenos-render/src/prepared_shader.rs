//! CPU-owned shader preparation shared by backend implementations.
//!
//! A prepared shader contains no GPU handles or paths. Its WGSL is returned to
//! the creating backend rather than kept in the cache; another backend can
//! lower the same decoded program and options on demand. This does not select
//! a backend, compile Metal, or imply support for a complete game pipeline.

use xenos_shader::{Shader, ShaderStage, Translation, TranslationOptions, UcodeError};

pub struct PreparedShader {
    pub(crate) shader: Shader,
    /// Binding/semantic metadata only. Source is deliberately not retained.
    pub(crate) translation: Translation,
    options: TranslationOptions,
}

impl PreparedShader {
    /// Decode and translate without a GPU device. The returned source and
    /// metadata originate from the same translation, not a parallel emitter.
    pub fn new(stage: ShaderStage, code: &[u32], options: &TranslationOptions) -> Result<(Self, String), UcodeError> {
        Ok(Self::from_decoded(Shader::new(stage, code)?, options))
    }

    pub(crate) fn from_decoded(shader: Shader, options: &TranslationOptions) -> (Self, String) {
        let mut translation = shader.translate(options);
        let source = std::mem::take(&mut translation.wgsl);
        (Self { shader, translation, options: options.clone() }, source)
    }

    pub fn shader(&self) -> &Shader {
        &self.shader
    }
    pub fn metadata(&self) -> &Translation {
        &self.translation
    }
    pub fn options(&self) -> &TranslationOptions {
        &self.options
    }

    /// Reproduce source for another backend or an explicit diagnostic. The
    /// normal WebGPU cache hit never calls this or retranslates the shader.
    pub fn wgsl(&self) -> String {
        self.shader.translate(&self.options).wgsl
    }

    /// CPU lowering/reflection only. Unsupported guest semantics must remain
    /// visible at the native boundary even when a syntactically valid shader
    /// could otherwise be emitted for them. The legacy backend is unchanged.
    #[cfg(feature = "metal-shaders")]
    pub fn metal(&self) -> Result<xenos_shader::msl::MetalShader, NativeShaderError> {
        if self.translation.uses_memexport || !self.translation.warnings.is_empty() {
            return Err(NativeShaderError::UnsupportedSemantics);
        }
        xenos_shader::msl::lower_wgsl(&self.wgsl(), self.translation.stage, self.translation.entry_point)
            .map_err(NativeShaderError::Lowering)
    }
}

#[cfg(feature = "metal-shaders")]
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum NativeShaderError {
    UnsupportedSemantics,
    Lowering(xenos_shader::msl::Error),
}

#[cfg(test)]
mod tests {
    use super::*;
    // Public, authored Xenos instructions: position/color export and color copy.
    const VS: [u32; 12] =
        [0x00013001, 0x00002200, 0, 0x00081000, 0x00390A88, 3, 0xC80F803E, 0, 0xE2010100, 0xC80F8000, 0, 0x22000000];
    const PS: [u32; 6] = [0x00001001, 0x00002200, 0, 0xC80F8000, 0, 0xE2000000];

    #[test]
    fn legacy_source_and_metadata_survive_source_release() {
        for (stage, code) in [(ShaderStage::Vertex, VS.as_slice()), (ShaderStage::Pixel, PS.as_slice())] {
            for options in [
                TranslationOptions::default(),
                TranslationOptions {
                    vs_output_interpolators: 1,
                    ps_interpolator_count: Some(1),
                    texture_binding_base: 96,
                    ..Default::default()
                },
            ] {
                let original = Shader::new(stage, code).unwrap().translate(&options);
                let (prepared, source) = PreparedShader::new(stage, code, &options).unwrap();
                assert_eq!(source, original.wgsl);
                assert_eq!(prepared.wgsl(), source);
                assert!(prepared.metadata().wgsl.is_empty());
                assert_eq!(
                    format!("{:?}", prepared.metadata()),
                    format!("{:?}", Translation { wgsl: String::new(), ..original })
                );
                assert_eq!(prepared.shader().program().ucode, code);
                assert_eq!(prepared.options(), &options);
            }
        }
    }

    #[test]
    fn preparation_owns_its_input_and_keeps_decode_refusals() {
        let mut code = PS.to_vec();
        let mut options = TranslationOptions { ps_interpolator_count: Some(1), ..Default::default() };
        let (prepared, source) = PreparedShader::new(ShaderStage::Pixel, &code, &options).unwrap();
        code.fill(0);
        options.ps_interpolator_count = Some(0);
        assert_eq!(prepared.wgsl(), source);
        assert_ne!(prepared.options(), &options);
        assert!(matches!(PreparedShader::new(ShaderStage::Pixel, &[], &options), Err(UcodeError::Empty)));
        assert!(matches!(
            PreparedShader::new(ShaderStage::Pixel, &PS[..3], &options),
            Err(UcodeError::InstructionOutOfRange { .. })
        ));
    }

    #[cfg(feature = "metal-shaders")]
    #[test]
    fn native_lowering_has_the_same_source_reflection_and_semantic_guard() {
        for (stage, code) in [(ShaderStage::Vertex, VS.as_slice()), (ShaderStage::Pixel, PS.as_slice())] {
            let options =
                TranslationOptions { vs_output_interpolators: 1, ps_interpolator_count: Some(1), ..Default::default() };
            let (mut prepared, source) = PreparedShader::new(stage, code, &options).unwrap();
            let expected = xenos_shader::msl::lower_wgsl(&source, stage, prepared.metadata().entry_point).unwrap();
            let actual = prepared.metal().unwrap();
            assert_eq!(actual.source, expected.source);
            assert_eq!(actual.entry_point, expected.entry_point);
            assert_eq!(actual.bindings, expected.bindings);
            assert_eq!(actual.runtime_sizes, expected.runtime_sizes);
            assert_eq!(actual.sizes_buffer_slot, expected.sizes_buffer_slot);
            prepared.translation.uses_memexport = true;
            assert_eq!(prepared.metal().unwrap_err(), NativeShaderError::UnsupportedSemantics);
            prepared.translation.uses_memexport = false;
            prepared.translation.warnings.push("unsupported authored semantics".into());
            assert_eq!(prepared.metal().unwrap_err(), NativeShaderError::UnsupportedSemantics);
        }
    }
}

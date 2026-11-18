// Runs the pinned real converter and SPIRV-Cross on authored shader inputs.
// No Metal API, game shader, cache implementation, or iPhone timing is simulated.
#include "SPIRVToMSLConverter.h"
#include "FileSupport.h"
#include "converter_inputs.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

// The original converter's optional file-export method is linked but is outside
// this CPU test. Accidentally reaching it must fail, never pretend to save.
namespace mvk {
std::string absolutePath(const std::string&) { std::abort(); }
bool writeFile(const std::string&,const std::vector<char>&,std::string&) { std::abort(); }
}

using namespace mvk;
using namespace spirv_cross;
struct Observation {
  SPIRVToMSLConversionConfiguration config;
  SPIRVToMSLConversionResult result;
};
static SPIRVToMSLConversionConfiguration configuration(unsigned index) {
  SPIRVToMSLConversionConfiguration config;
  config.options.entryPointName="main";
  config.options.entryPointStage=spv::ExecutionModelFragment;
  config.options.mslOptions.platform=CompilerMSL::Options::iOS;
  config.options.mslOptions.msl_version=CompilerMSL::Options::make_msl_version(index&1?3:2,index&1?0:3);
  for(unsigned binding=0;binding<2;++binding){
    mvk::MSLResourceBinding resource;
    resource.resourceBinding.stage=spv::ExecutionModelFragment;
    resource.resourceBinding.desc_set=0;
    resource.resourceBinding.binding=binding;
    resource.resourceBinding.msl_texture=binding+2*(index%4);
    resource.resourceBinding.msl_sampler=binding+2*(index%4);
    config.resourceBindings.push_back(resource);
  }
  mvk::MSLShaderInterfaceVariable output;
  output.shaderVar.location=0;
  config.shaderOutputs.push_back(output);
  return config;
}
static Observation convert(SPIRVToMSLConverter& converter,unsigned index) {
  Observation observed;
  observed.config=configuration(index);
  // Also exercise the real MSL/GLSL result logging. SPIRV-Tools disassembly is
  // explicitly disabled in this portable build, as supported by upstream.
  bool ok=converter.convert(observed.config,observed.result,false,index&2,index&4);
  assert(ok && !observed.result.msl.empty());
  return observed;
}
static std::string fingerprint(const Observation& value) {
  const auto& info=value.result.resultInfo;
  std::ostringstream out;
  out << value.result.msl << '\0' << value.result.resultLog << '\0';
  out << info.entryPoint.mtlFunctionName << ':' << info.entryPoint.fpFastMathFlags << ':';
  const auto& group=info.entryPoint.workgroupSize;
  for(const auto& dim:{group.width,group.height,group.depth})
    out << dim.size << ',' << dim.specializationID << ',' << dim.isSpecialized << ';';
  out << info.isRasterizationDisabled << info.isPositionInvariant << info.needsSwizzleBuffer
      << info.needsOutputBuffer << info.needsPatchOutputBuffer << info.needsBufferSizeBuffer
      << info.needsDynamicOffsetBuffer << info.needsInputThreadgroupMem << info.needsDispatchBaseBuffer
      << info.needsViewRangeBuffer << info.usesPhysicalStorageBufferAddressesCapability;
  for(const auto& pair:info.specializationMacros)
    out << ':' << pair.first << ':' << pair.second.name << ':' << pair.second.isFloat << ':' << pair.second.isSigned;
  for(const auto& input:value.config.shaderInputs)out << ':' << input.outIsUsedByShader;
  for(const auto& output:value.config.shaderOutputs)out << ':' << output.outIsUsedByShader;
  for(const auto& binding:value.config.resourceBindings)out << ':' << binding.outIsUsedByShader;
  return out.str();
}
// Characterize the pinned exception-enabled converter's existing error result.
// This is deliberately invalid authored SPIR-V, not a valid game shader or a fix.
// Returning true after logging a conversion error must not be called success.
static void characterizeCaughtSourceError() {
  SPIRVToMSLConverter converter;
  converter.setSPIRV(std::vector<uint32_t>{});
  SPIRVToMSLConversionConfiguration config;
  SPIRVToMSLConversionResult result;
  const bool returnedSuccess=converter.convert(config,result,false,false,false);
  const bool loggedError=result.resultLog.find("SPIR-V to MSL conversion error")!=std::string::npos;
  assert(returnedSuccess && loggedError && result.msl.empty());
  puts("known_pinned_converter_error returned_success=1 empty_msl=1 error_logged=1 production_fix=0");
}
int main() {
  characterizeCaughtSourceError();
  std::array<SPIRVToMSLConverter,3> converters;
  const std::array<std::vector<uint32_t>,3> inputs={input0,input1,input2};
  std::array<std::mutex,3> locks;
  std::vector<Observation> expected;
  std::vector<std::string> expectedBytes;
  for(unsigned program=0;program<3;++program){
    converters[program].setSPIRV(inputs[program]);
    for(unsigned option=0;option<8;++option){
      expected.push_back(convert(converters[program],option));
      expectedBytes.push_back(fingerprint(expected.back()));
    }
  }
  for(bool serialized:{true,false}){
    std::atomic<int> arrived{0};
    std::vector<std::future<void>> workers;
    for(unsigned thread=0;thread<8;++thread)workers.push_back(std::async(std::launch::async,[&,thread]{
      ++arrived;while(arrived.load()<8)std::this_thread::yield();
      for(unsigned iteration=0;iteration<32;++iteration){
        unsigned index=(iteration*7+thread)%24,program=index/8,option=index%8;
        std::unique_lock<std::mutex> lock(locks[program],std::defer_lock);
        if(serialized)lock.lock();
        auto observed=convert(converters[program],option);
        assert(observed.config.matches(expected[index].config));
        assert(expected[index].config.matches(observed.config));
        assert(fingerprint(observed)==expectedBytes[index]);
      }
    }));
    for(auto& worker:workers)worker.get();
    for(unsigned program=0;program<3;++program)assert(converters[program].getSPIRV()==inputs[program]);
    std::printf("real_converter serialized=%d threads=8 cases=24 calls=256 msl_and_metadata_mismatches=0 input_mutations=0\n",serialized);
  }
}

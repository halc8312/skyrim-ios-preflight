// Authored boundaries for extracted pinned methods, not a Metal implementation.
// Included after the existing hot-cache fixture with its additions assembled by
// test_mvk_module_cold.py. The actual convert() body is compiled unmodified apart
// from the candidate's lock, including its original optional diagnostic branch.
#pragma once
#include <cstdio>
#include <climits>
#include <sys/stat.h>
#include <string>
namespace spv { enum Stage { ExecutionModelVertex, ExecutionModelTessellationControl,
  ExecutionModelTessellationEvaluation, ExecutionModelFragment, ExecutionModelGeometry,
  ExecutionModelTaskNV, ExecutionModelMeshNV, ExecutionModelGLCompute }; }
struct FixtureConfig { bool debugMode=false, shaderLogEstimatedGLSL=false; const char* shaderDumpDir=nullptr; };
inline FixtureConfig fixtureConfig;
inline const FixtureConfig& getMVKConfig() { return fixtureConfig; }
template<class... T> void MVKLogInfo(const char*, T...) {}
template<class... T> VkResult reportError(VkResult result, const char*, T...) { return result; }

struct FixtureConverter {
  std::atomic<int> inside{0}, maximum{0};
  std::vector<uint32_t> spirv;
  const std::vector<uint32_t>& getSPIRV() { return spirv; }
  bool convert(SPIRVToMSLConversionConfiguration& config, SPIRVToMSLConversionResult& out, bool, bool, bool) {
    int current=++inside, old=maximum.load();
    while(old<current && !maximum.compare_exchange_weak(old,current)) {}
    struct Exit { std::atomic<int>& count; ~Exit(){--count;} } exit{inside};
    current=++fixture.active;old=fixture.maximum.load();
    while(old<current && !fixture.maximum.compare_exchange_weak(old,current)) {}
    ++fixture.compiles; fixture.waitAt(config.key,false);
    int failure=fixture.failConvert.exchange(0);
    if(failure){--fixture.active;if(failure==2)throw std::runtime_error("authored conversion exception");return false;}
    out.key=config.key; return true;
  }
};
struct MVKShaderModule {
  int id;
  explicit MVKShaderModule(int value):id(value){}
  int getKey(){return id;}
  std::mutex _conversionLock, _accessLock;
  FixtureConverter _spvConverter;
  struct {size_t codeHash=0;} _key;
  struct Stats {struct {int spirvToMSL=0;} shaderCompilation;} stats;
  Stats& getPerformanceStats(){return stats;}
  uint64_t getPerformanceTimestamp(){return 0;}
  void addPerformanceInterval(int,uint64_t){}
  bool convert(SPIRVToMSLConversionConfiguration*,SPIRVToMSLConversionResult&);
};
struct MVKMetalCompiler {
  MVKVulkanAPIDeviceObject* _owner;
  std::string _compilerType="authored-boundary";
  struct Error {long code=1;struct {const char* UTF8String="injected failure";} localizedDescription;} _compileError;
  explicit MVKMetalCompiler(MVKVulkanAPIDeviceObject* owner):_owner(owner){}
  void handleError(); // Exact pinned or candidate body compiled by the harness.
};
inline std::atomic<bool> fixtureLibraryFailure{false},fixtureLibraryThrow{false};
inline MVKShaderLibrary::MVKShaderLibrary(MVKPipelineCache* owner,const SPIRVToMSLConversionResult& value):key(value.key) {
  if(fixtureLibraryThrow.exchange(false)){--fixture.active;throw std::bad_alloc();}
  ++fixture.live;fixture.waitAt(key,true);
  if(fixtureLibraryFailure.load()){valid=false;MVKMetalCompiler compiler(owner);compiler.handleError();}
  --fixture.active;
}

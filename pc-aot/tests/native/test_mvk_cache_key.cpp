// Authored configuration inputs against the real pinned converter comparisons.
#include "SPIRVToMSLConverter.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <set>
#include <tuple>
using namespace mvk;
using namespace spv;
using Key = std::tuple<unsigned, unsigned, unsigned>;
static std::set<Key> active(const SPIRVToMSLConversionConfiguration& c) {
  std::set<Key> result;
  for (const auto& d : c.dynamicBufferDescriptors)
    if (d.stage == c.options.entryPointStage)
      result.emplace(d.descriptorSet, d.binding, d.index);
  return result;
}
static SPIRVToMSLConversionConfiguration config(unsigned bits, unsigned sets, ExecutionModel stage) {
  SPIRVToMSLConversionConfiguration c;
  c.options.entryPointStage = stage;
  c.options.entryPointName = "main";
  const DescriptorBinding descriptors[] = {
    {ExecutionModelFragment, 0, 8, 0}, {ExecutionModelFragment, 0, 8, 1},
    {ExecutionModelFragment, 1, 9, 0}, {ExecutionModelVertex, 0, 8, 0}};
  for (unsigned i=0; i<4; ++i) if (bits & (1u<<i)) c.dynamicBufferDescriptors.push_back(descriptors[i]);
  for (unsigned i=0; i<2; ++i) if (sets & (1u<<i)) c.discreteDescriptorSets.push_back(i);
  return c;
}
int main() {
  unsigned cases=0, false_hits=0;
  for (auto stage : {ExecutionModelFragment, ExecutionModelVertex, ExecutionModelGLCompute})
    for (unsigned a=0; a<16; ++a) for (unsigned b=0; b<16; ++b)
      for (unsigned x=0; x<4; ++x) for (unsigned y=0; y<4; ++y) {
        auto left=config(a,x,stage), right=config(b,y,stage);
        bool oracle=active(left)==active(right) && x==y;
        bool matches=left.matches(right);
        if (matches && !oracle) ++false_hits;
        assert(!oracle || matches);
#if FIXTURE_NEW
        assert(matches==oracle);
#endif
        std::reverse(right.dynamicBufferDescriptors.begin(),right.dynamicBufferDescriptors.end());
        std::reverse(right.discreteDescriptorSets.begin(),right.discreteDescriptorSets.end());
        assert(matches==left.matches(right)); // order must remain irrelevant
        ++cases;
      }
  auto base=config(1,0,ExecutionModelFragment), other=base;
  other.dynamicBufferDescriptors.push_back(other.dynamicBufferDescriptors.front());
  assert(base.matches(other) && other.matches(base)); // semantic sets, not vector sizes
  other=base; other.options.entryPointName="different"; assert(!base.matches(other));
  other=base; other.options.entryPointStage=ExecutionModelVertex; assert(!base.matches(other));
  other=base; other.options.shouldFlipVertexY=!base.options.shouldFlipVertexY; assert(!base.matches(other));
  // Resources unused by the cached shader are intentionally permitted; this is
  // not a blanket equality change to all conversion configuration vectors.
  other=base; MSLResourceBinding resource;
  resource.resourceBinding.stage=ExecutionModelFragment; resource.resourceBinding.binding=7;
  other.resourceBindings.push_back(resource); assert(base.matches(other) && other.matches(base));
  other.resourceBindings[0].outIsUsedByShader=true; assert(base.matches(other) && !other.matches(base));
  auto static_shader=config(0,0,ExecutionModelFragment), dynamic_shader=config(1,0,ExecutionModelFragment);
#if FIXTURE_NEW
  assert(!static_shader.matches(dynamic_shader)); assert(false_hits==0);
#else
  assert(static_shader.matches(dynamic_shader)); assert(false_hits>0);
#endif
  std::printf("real_mvk_cache_key=PASS cases=%u incompatible_hits=%u new=%d\n",cases,false_hits,FIXTURE_NEW);
}

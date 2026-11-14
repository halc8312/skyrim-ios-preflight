// Actual pinned metadata types/constructor, authored Objective-C retain boundary.
#include "metadata.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
static thread_local bool measuring=false;
static thread_local size_t allocations=0;
// Keep replacement allocation boundaries visible to the counting fixture.
// Inlining delete alone makes GCC misdiagnose the matching malloc-backed new.
__attribute__((noinline)) void* operator new(size_t n){if(measuring)++allocations;if(void* p=malloc(n?n:1))return p;throw std::bad_alloc();}
__attribute__((noinline)) void operator delete(void* p)noexcept{free(p);}
__attribute__((noinline)) void operator delete(void* p,size_t)noexcept{free(p);}
static SPIRVToMSLConversionResultInfo value(unsigned entries,bool longName,bool flags){
  SPIRVToMSLConversionResultInfo r={};
  r.entryPoint.mtlFunctionName=longName?std::string(80,'x'):"main0";
  r.entryPoint.fpFastMathFlags=13;
  r.entryPoint.workgroupSize={{3,7,true},{4,8,false},{5,9,true}};
  r.isRasterizationDisabled=r.isPositionInvariant=r.needsSwizzleBuffer=flags;
  r.needsOutputBuffer=r.needsPatchOutputBuffer=r.needsBufferSizeBuffer=flags;
  r.needsDynamicOffsetBuffer=r.needsInputThreadgroupMem=r.needsDispatchBaseBuffer=flags;
  r.needsViewRangeBuffer=r.usesPhysicalStorageBufferAddressesCapability=flags;
  for(unsigned i=0;i<entries;++i)r.specializationMacros[i]={"macro_"+std::to_string(i),i%2==0,i%3==0};
  return r;
}
static void same(const SPIRVToMSLConversionResultInfo& a,const SPIRVToMSLConversionResultInfo& b){
  assert(a.entryPoint.mtlFunctionName==b.entryPoint.mtlFunctionName);
  assert(a.entryPoint.fpFastMathFlags==b.entryPoint.fpFastMathFlags);
  const auto* aa=&a.entryPoint.workgroupSize;const auto* bb=&b.entryPoint.workgroupSize;
#define DIM(d) assert(aa->d.size==bb->d.size&&aa->d.specializationID==bb->d.specializationID&&aa->d.isSpecialized==bb->d.isSpecialized)
  DIM(width);DIM(height);DIM(depth);
#undef DIM
#define FIELD(f) assert(a.f==b.f)
  FIELD(isRasterizationDisabled);FIELD(isPositionInvariant);FIELD(needsSwizzleBuffer);
  FIELD(needsOutputBuffer);FIELD(needsPatchOutputBuffer);FIELD(needsBufferSizeBuffer);
  FIELD(needsDynamicOffsetBuffer);FIELD(needsInputThreadgroupMem);FIELD(needsDispatchBaseBuffer);
  FIELD(needsViewRangeBuffer);FIELD(usesPhysicalStorageBufferAddressesCapability);
#undef FIELD
  assert(a.specializationMacros.size()==b.specializationMacros.size());
  for(const auto& p:a.specializationMacros){const auto& q=b.specializationMacros.at(p.first);assert(p.second.name==q.name&&p.second.isFloat==q.isFloat&&p.second.isSigned==q.isSigned);}
}
int main(){
  unsigned cases=0;FixtureFunction function;
  for(unsigned entries:{0u,1u,8u,32u})for(bool longName:{false,true})for(bool flags:{false,true}){
    auto input=value(entries,longName,flags),expected=input;
    // Account for library-specific map sentinel allocations independently.
    // The old signature adds a copy construction before the same member's
    // default construction and assignment; the borrowed signature omits it.
    allocations=0;measuring=true;
    {SPIRVToMSLConversionResultInfo argument(input);same(argument,expected);}
    measuring=false;size_t argumentAllocations=allocations;
    allocations=0;measuring=true;
    {SPIRVToMSLConversionResultInfo member;member=input;same(member,expected);}
    measuring=false;size_t memberAllocations=allocations;
    allocations=0;measuring=true;
    {
      MVKMTLFunction f(&function,input,MTLSize{7,8,9});
      measuring=false;
      assert(allocations==memberAllocations+(FIXTURE_NEW?0:argumentAllocations));
      if(entries)assert(argumentAllocations>=entries);
      assert(function.references==2&&f._mtlFunction==&function);
      assert(f.threadGroupSize.width==7&&f.threadGroupSize.height==8&&f.threadGroupSize.depth==9);
      same(f.shaderConversionResults,expected);
      input=value(0,false,!flags);same(f.shaderConversionResults,expected);
    }
    assert(function.references==1);
    {MVKMTLFunction temporary(nullptr,value(entries,longName,flags),MTLSize{1,1,1});same(temporary.shaderConversionResults,expected);}
    ++cases;
  }
  printf("actual_metadata=PASS cases=%u owned_copies=%u retain_lifetime=PASS\n",cases,FIXTURE_NEW?1:2);
}

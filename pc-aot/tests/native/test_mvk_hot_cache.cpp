#include "mvk_hot_cache_fixture.h"
#include <cstdio>
#if FIXTURE_NEW
#include "mvk_convert_observation.h"
#endif
static MVKShaderLibrary* request(MVKPipelineCache& cache,int module,int key,bool refuse=false,VkPipelineCreationFeedback* feedback=nullptr) {
  SPIRVToMSLConversionConfiguration config{key}; MVKShaderModule shader{module}; MVKPipeline pipeline{refuse};
  auto* result=cache.getShaderLibrary(&config,&shader,&pipeline,feedback,0);
  if(result)assert(result->key==key);
  return result;
}
static void whileCold(bool inLibrary) {
  MVKPipelineCache cache;
  auto* warm=request(cache,1,100);
  assert(warm && cache.snapshot()==1);
  int before=fixture.compiles.load();fixture.arm(200,inLibrary);
  auto cold=async(launch::async,[&]{return request(cache,2,200);});fixture.waitEntered();
  auto hot=async(launch::async,[&]{return request(cache,1,100);});
  auto snapshot=async(launch::async,[&]{return cache.snapshot();});
#if FIXTURE_NEW
  assert(hot.wait_for(chrono::seconds(2))==future_status::ready);
  assert(snapshot.wait_for(chrono::seconds(2))==future_status::ready);
  assert(hot.get()==warm && snapshot.get()==1);
#else
  assert(hot.wait_for(chrono::milliseconds(40))==future_status::timeout);
  assert(snapshot.wait_for(chrono::milliseconds(40))==future_status::timeout);
#endif
  vector<future<MVKShaderLibrary*>> duplicates;
  for(int i=0;i<8;++i)duplicates.push_back(async(launch::async,[&]{return request(cache,2,200);}));
  auto otherCold=async(launch::async,[&]{return request(cache,7,700);});
  assert(otherCold.wait_for(chrono::milliseconds(40))==future_status::timeout);
  assert(fixture.compiles.load()==before+1); // a different cold key stays serialized too
  fixture.release();auto* ready=cold.get();assert(ready);
  for(auto& value:duplicates)assert(value.get()==ready);
  assert(otherCold.get());
#if !FIXTURE_NEW
  assert(hot.get()==warm);auto count=snapshot.get();assert(count==2 || count==3);
#endif
  assert(cache.snapshot()==3 && fixture.compiles.load()==before+2);
  assert(fixture.maximum.load()==1);
}
static void failures() {
  MVKPipelineCache cache;int before=fixture.compiles.load();
  VkPipelineCreationFeedback fb;
  assert(!request(cache,3,300,true,&fb));assert(fixture.compiles.load()==before);
  assert(fb.flags==VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT);
  fixture.failConvert=1;assert(!request(cache,3,300));assert(cache.snapshot()==0);
  assert(request(cache,3,300) && cache.snapshot()==1);
  cache._isExternallySynchronized=true;
  auto* external=request(cache,4,400);assert(external && request(cache,4,400)==external);
#if FIXTURE_NEW
  cache._isExternallySynchronized=false;
  int live=fixture.live.load();
  cache.getShaderLibraryCache(5)->_shaderLibraries.fail=true;
  bool caught=false;try{request(cache,5,500);}catch(const bad_alloc&){caught=true;}
  assert(caught && fixture.live.load()==live && cache.snapshot()==2);
  assert(request(cache,5,500) && cache.snapshot()==3);
#endif
}
#if FIXTURE_NEW
static void publicationDuringCompile() {
  MVKPipelineCache cache;fixture.arm(600,true);
  auto cold=async(launch::async,[&]{return request(cache,6,600);});fixture.waitEntered();
  cache.publishReadyForTest(6,600);auto* merged=request(cache,6,600);
  assert(cache.snapshot()==1);
  fixture.release();assert(cold.get()==merged);
  assert(cache.snapshot()==1 && fixture.live.load()==1);
}
#endif
static void growingVariantStorage() {
  MVKPipelineCache cache;
  vector<MVKShaderLibrary*> expected;
  for(int i=0;i<257;++i)expected.push_back(request(cache,11,10000+i));
  int before=fixture.compiles.load();
  vector<future<void>> readers;
  for(int thread=0;thread<4;++thread)readers.push_back(async(launch::async,[&,thread]{
    for(int n=0;n<1024;++n){int i=(n*17+thread)%257;assert(request(cache,11,10000+i)==expected[i]);}
  }));
  // Grow the real MVKSmallVector while existing variants are looked up. The
  // list lock protects storage movement; the independently owned libraries
  // must retain their identities after each container reallocation.
  for(int i=257;i<513;++i)assert(request(cache,11,10000+i));
  for(auto& reader:readers)reader.get();
  assert(fixture.compiles.load()==before+256 && cache.snapshot()==513);
}
int main(){
  whileCold(false);assert(fixture.live==0);
  whileCold(true);assert(fixture.live==0);
  failures();assert(fixture.live==0);
  growingVariantStorage();assert(fixture.live==0);
#if FIXTURE_NEW
  publicationDuringCompile();assert(fixture.live==0);
  conversionObservation([](MVKPipelineCache& cache,int module,int key,bool refuse){return request(cache,module,key,refuse);});
  assert(fixture.live==0);
#endif
  assert(fixture.active==0);
  printf("actual_cache_method=PASS hot_during_cold=%d snapshot_during_cold=%d max_cold=%d duplicate_compiles=0 public_cpu_only=1\n",FIXTURE_NEW,FIXTURE_NEW,int(fixture.maximum));
}

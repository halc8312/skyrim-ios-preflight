#include "mvk_module_fixture.h"
#include "mvk_module_cold_support.h"
#include <cstdio>

static bool moduleEnabled;

static MVKShaderLibrary* request(MVKPipelineCache& cache,MVKShaderModule& shader,int key,bool refuse=false,VkPipelineCreationFeedback* feedback=nullptr) {
  SPIRVToMSLConversionConfiguration config{key};MVKPipeline pipeline{refuse};
  auto* result=cache.getShaderLibrary(&config,&shader,&pipeline,feedback,0);
  if(result)assert(result->key==key);
  return result;
}
static MVKShaderLibrary* request(MVKPipelineCache& cache,int module,int key,bool refuse=false,VkPipelineCreationFeedback* feedback=nullptr) {
  MVKShaderModule shader(module);return request(cache,shader,key,refuse,feedback);
}
static void coldIsolation(bool library) {
  MVKPipelineCache cache;
  auto* warm=request(cache,1,100);assert(cache.snapshot()==1);
  fixture.maximum=0;int before=fixture.compiles.load();fixture.arm(101,library);
  auto cold=async(launch::async,[&]{return request(cache,1,101);});fixture.waitEntered();
  auto hot=async(launch::async,[&]{return request(cache,1,100);});
  auto snapshot=async(launch::async,[&]{return cache.snapshot();});
  assert(hot.wait_for(chrono::seconds(2))==future_status::ready && hot.get()==warm);
  assert(snapshot.wait_for(chrono::seconds(2))==future_status::ready && snapshot.get()==1);
  vector<future<MVKShaderLibrary*>> duplicates;
  for(int i=0;i<8;++i)duplicates.push_back(async(launch::async,[&]{return request(cache,1,101);}));
  auto sameModule=async(launch::async,[&]{return request(cache,1,102);});
  auto otherModule=async(launch::async,[&]{return request(cache,2,200);});
  assert(sameModule.wait_for(chrono::milliseconds(40))==future_status::timeout);
  if(moduleEnabled){
    assert(otherModule.wait_for(chrono::seconds(2))==future_status::ready);
    assert(otherModule.get() && fixture.compiles==before+2 && cache.snapshot()==2);
  }else{
    assert(otherModule.wait_for(chrono::milliseconds(40))==future_status::timeout);
    assert(fixture.compiles==before+1);
  }
  fixture.release();auto* ready=cold.get();assert(ready);
  for(auto& waiter:duplicates)assert(waiter.get()==ready);
  assert(sameModule.get());
  if(!moduleEnabled)assert(otherModule.get());
  assert(cache.snapshot()==4 && fixture.compiles==before+3);
  assert(fixture.maximum==(moduleEnabled?2:1));
}
static void failuresAndPublication() {
  MVKPipelineCache cache;VkPipelineCreationFeedback fb;int before=fixture.compiles.load();
  assert(!request(cache,3,300,true,&fb) && fixture.compiles==before);
  assert(fb.flags==VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT);
  fixture.failConvert=1;assert(!request(cache,3,300) && cache.snapshot()==0);
  fixture.failConvert=2;bool caught=false;
  try{request(cache,3,300);}catch(const runtime_error&){caught=true;}
  assert(caught && cache.snapshot()==0);
  fixtureLibraryThrow=true;caught=false;
  try{request(cache,3,300);}catch(const bad_alloc&){caught=true;}
  assert(caught && fixture.live==0 && cache.snapshot()==0);
  cache.getShaderLibraryCache(3)->_shaderLibraries.fail=true;caught=false;
  try{request(cache,3,300);}catch(const bad_alloc&){caught=true;}
  assert(caught && fixture.live==0 && cache.snapshot()==0);
  assert(request(cache,3,300));
  cache._isExternallySynchronized=true;auto* external=request(cache,4,400);
  assert(external && request(cache,4,400)==external);cache._isExternallySynchronized=false;
  fixture.arm(600,true);auto pending=async(launch::async,[&]{return request(cache,6,600);});fixture.waitEntered();
  cache.publishReadyForTest(6,600);auto* merged=request(cache,6,600);assert(cache.snapshot()==3);
  fixture.release();assert(pending.get()==merged && fixture.live==3);
  fixtureLibraryFailure=true;
  vector<future<MVKShaderLibrary*>> failed;
  for(int i=0;i<8;++i)failed.push_back(async(launch::async,[&,i]{return request(cache,20+i,2000+i);}));
  for(int i=0;i<8;++i){auto* value=failed[i].get();assert(value && !value->valid && request(cache,20+i,2000+i)==value);}
  assert(cache.getConfigurationResult()==VK_ERROR_INITIALIZATION_FAILED);
  fixtureLibraryFailure=false;
}
static void growingStorage() {
  MVKPipelineCache cache;vector<MVKShaderLibrary*> expected;
  for(int i=0;i<257;++i)expected.push_back(request(cache,10,10000+i));
  int before=fixture.compiles;vector<future<void>> readers;
  for(int t=0;t<4;++t)readers.push_back(async(launch::async,[&,t]{
    for(int n=0;n<1024;++n){int i=(n*17+t)%257;assert(request(cache,10,10000+i)==expected[i]);}
  }));
  for(int i=257;i<513;++i)assert(request(cache,10,10000+i));
  for(auto& reader:readers)reader.get();
  assert(cache.snapshot()==513 && fixture.compiles==before+256);
}
#if CANDIDATE
static void converterAcrossCaches(bool diagnostics) {
  fixtureConfig.debugMode=diagnostics;
  MVKPipelineCache first,second;MVKShaderModule module(8);
  fixture.arm(800,false);auto a=async(launch::async,[&]{return request(first,module,800);});fixture.waitEntered();
  auto b=async(launch::async,[&]{return request(second,module,801);});
  auto cacheFree=async(launch::async,[&]{
    lock_guard<mutex> lock(module._accessLock);SPIRVToMSLConversionConfiguration config{802};SPIRVToMSLConversionResult result;
    assert(module.convert(&config,result));assert(result.key==802);--fixture.active;
  });
  if(diagnostics){
    assert(b.wait_for(chrono::milliseconds(40))==future_status::timeout);
    assert(cacheFree.wait_for(chrono::milliseconds(40))==future_status::timeout);
  }else{
    assert(b.wait_for(chrono::seconds(2))==future_status::ready);
    assert(cacheFree.wait_for(chrono::seconds(2))==future_status::ready);
    assert(module._spvConverter.maximum>=2);
  }
  assert(module._spvConverter.inside==1);
  fixture.release();assert(a.get() && b.get());cacheFree.get();
  if(diagnostics)assert(module._spvConverter.maximum==1);
  fixture.arm(803,true);auto slow=async(launch::async,[&]{return request(first,module,803);});fixture.waitEntered();
  auto independent=async(launch::async,[&]{return request(second,module,804);});
  assert(independent.wait_for(chrono::seconds(2))==future_status::ready && independent.get());
  fixture.release();assert(slow.get());
  if(diagnostics)assert(module._spvConverter.maximum==1);
  fixtureConfig.debugMode=false;
}
static void concurrentErrorPublication() {
  for(int round=0;round<32;++round){
    MVKPipelineCache cache;vector<future<void>> errors;atomic<int> arrived{0};
    for(int t=0;t<8;++t)errors.push_back(async(launch::async,[&]{
      ++arrived;while(arrived<8)this_thread::yield();
      MVKMetalCompiler compiler(&cache);for(int n=0;n<64;++n)compiler.handleError();
    }));
    for(auto& error:errors){error.get();}
    assert(cache.getConfigurationResult()==VK_ERROR_INITIALIZATION_FAILED);
    cache.recordShaderCompilationResult(-99);assert(cache.getConfigurationResult()==VK_ERROR_INITIALIZATION_FAILED);
  }
  MVKVulkanAPIDeviceObject nonCache;MVKMetalCompiler compiler(&nonCache);compiler.handleError();
  assert(nonCache.getConfigurationResult()==VK_ERROR_INITIALIZATION_FAILED);
}
#endif
int main(int argc,char** argv){
  assert(argc==2 && (std::strcmp(argv[1],"0")==0 || std::strcmp(argv[1],"1")==0));
  moduleEnabled=std::strcmp(argv[1],"1")==0;
  coldIsolation(false);assert(fixture.live==0 && fixture.active==0);
#if CANDIDATE
  // Reconfiguration after first cold use cannot mix global/module mutexes.
  assert(!setenv("SKYRIM_MVK_MODULE_COLD",moduleEnabled?"0":"1",1));
#endif
  coldIsolation(true);assert(fixture.live==0 && fixture.active==0);
  failuresAndPublication();assert(fixture.live==0 && fixture.active==0);
  growingStorage();assert(fixture.live==0 && fixture.active==0);
#if CANDIDATE
  converterAcrossCaches(false);assert(fixture.live==0 && fixture.active==0);
  converterAcrossCaches(true);assert(fixture.live==0 && fixture.active==0);
  concurrentErrorPublication();
#endif
  printf("module_cold_candidate=%d ready_hit=PASS catalog_snapshot=PASS other_module_progress=%d same_module_cold_serial=PASS duplicate_compiles=0 publication_failure_cleanup=PASS vector_growth_reads=4096\n",CANDIDATE,int(moduleEnabled));
#if CANDIDATE
  puts("actual_convert_body=PASS normal_cross_cache_progress=PASS diagnostic_exclusion=PASS compile_error_reports=16384 first_error_preserved=PASS public_cpu_only=1");
#endif
}

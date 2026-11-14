// Authored CPU interfaces for the actual cache method bodies. No Metal device
// or real driver compilation is represented by this fixture.
#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include "MVKSmallVector.h"
using namespace std;
struct SPIRVToMSLConversionConfiguration {
  int key=0;
  bool matches(const SPIRVToMSLConversionConfiguration& b) const {return key==b.key;}
  void alignWith(const SPIRVToMSLConversionConfiguration& b) {key=b.key;}
};
struct SPIRVToMSLConversionResult {int key=0;};
struct VkPipelineCreationFeedback {uint32_t flags=0; uint64_t duration=0;};
constexpr uint32_t VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT=1;
inline void mvkEnableFlags(uint32_t& flags,uint32_t bit){flags|=bit;}
inline uint64_t mvkGetElapsedNanoseconds(uint64_t){return 1;}
using MVKShaderModuleKey=int;
struct FixtureControl {
  mutex lock; condition_variable cv;
  int blocked=0; bool inLibrary=false,entered=false,released=false;
  atomic<int> active{0},maximum{0},compiles{0},live{0},failConvert{0};
  void waitAt(int key,bool library) {
    unique_lock<mutex> guard(lock);
    if (key!=blocked || library!=inLibrary) return;
    entered=true;cv.notify_all();cv.wait(guard,[&]{return released;});
  }
  void arm(int key,bool library) {lock_guard<mutex> guard(lock);blocked=key;inLibrary=library;entered=false;released=false;}
  void waitEntered(){unique_lock<mutex> guard(lock);assert(cv.wait_for(guard,chrono::seconds(3),[&]{return entered;}));}
  void release(){lock_guard<mutex> guard(lock);released=true;cv.notify_all();}
};
inline FixtureControl fixture;
class MVKPipelineCache;
struct MVKShaderLibrary {
  int key;
  MVKShaderLibrary(MVKPipelineCache*,const SPIRVToMSLConversionResult& value):key(value.key) {
    ++fixture.live; fixture.waitAt(key,true);
    --fixture.active;
  }
  explicit MVKShaderLibrary(int merged):key(merged){++fixture.live;}
  void destroy(){delete this;}
  ~MVKShaderLibrary(){--fixture.live;}
};
struct MVKShaderModule {
  int id;
  int getKey(){return id;}
  bool convert(SPIRVToMSLConversionConfiguration* config,SPIRVToMSLConversionResult& out) {
    int count=++fixture.active;
    int old=fixture.maximum.load();while(old<count&&!fixture.maximum.compare_exchange_weak(old,count)){}
    ++fixture.compiles; fixture.waitAt(config->key,false);
    if(fixture.failConvert.exchange(0)){--fixture.active;return false;}
    out.key=config->key;return true;
  }
};
struct MVKPipeline {
  bool refuse=false;
  bool shouldFailOnPipelineCompileRequired(){return refuse;}
};
struct FixturePairs:MVKSmallVector<pair<SPIRVToMSLConversionConfiguration,MVKShaderLibrary*>> {
  using Storage=MVKSmallVector<pair<SPIRVToMSLConversionConfiguration,MVKShaderLibrary*>>;
  bool fail=false;
  void emplace_back(SPIRVToMSLConversionConfiguration c,MVKShaderLibrary* p) {
    if(fail){fail=false;throw bad_alloc();}
    Storage::emplace_back(c,p);
  }
};
class MVKShaderLibraryCache {
public:
  MVKPipelineCache* _owner;
  FixturePairs _shaderLibraries;
  explicit MVKShaderLibraryCache(MVKPipelineCache* p):_owner(p){}
  MVKShaderLibrary* getShaderLibrary(SPIRVToMSLConversionConfiguration*,MVKShaderModule*,MVKPipeline*,bool*,VkPipelineCreationFeedback*,uint64_t);
  MVKShaderLibrary* findShaderLibrary(SPIRVToMSLConversionConfiguration*,VkPipelineCreationFeedback* =nullptr,uint64_t=0);
  MVKShaderLibrary* addShaderLibrary(const SPIRVToMSLConversionConfiguration*,const SPIRVToMSLConversionResult&);
  struct Stats {struct {int shaderLibraryFromCache=0;} shaderCompilation;} stats;
  Stats& getPerformanceStats(){return stats;}
  void addPerformanceInterval(int,uint64_t){}
  ~MVKShaderLibraryCache(){for(auto& p:_shaderLibraries)p.second->destroy();}
};
class MVKPipelineCache {
public:
  map<int,MVKShaderLibraryCache*> _shaderCache;
  size_t _dataSize=0;
  mutex _shaderCacheLock,_shaderCompileLock;
  bool _isExternallySynchronized=false;
  MVKShaderLibrary* getShaderLibrary(SPIRVToMSLConversionConfiguration*,MVKShaderModule*,MVKPipeline*,VkPipelineCreationFeedback* =nullptr,uint64_t=0);
  MVKShaderLibrary* getShaderLibraryImpl(SPIRVToMSLConversionConfiguration*,MVKShaderModule*,MVKPipeline*,VkPipelineCreationFeedback*,uint64_t);
  MVKShaderLibraryCache* getShaderLibraryCache(MVKShaderModuleKey);
  void markDirty();
  // A snapshot takes the same lock as actual writeData; its count is cached in
  // the same _dataSize field. This does not emulate Cereal / driver data bytes.
  size_t snapshot(){lock_guard<mutex> lock(_shaderCacheLock);if(!_dataSize){
    size_t count=0;for(auto p:_shaderCache)count+=p.second->_shaderLibraries.size();_dataSize=count+1;
  }return _dataSize-1;}
  void mergeReady(int module,int key){lock_guard<mutex> lock(_shaderCacheLock);
    getShaderLibraryCache(module)->_shaderLibraries.emplace_back(SPIRVToMSLConversionConfiguration{key},new MVKShaderLibrary(key));markDirty();}
  ~MVKPipelineCache(){for(auto p:_shaderCache)delete p.second;}
};

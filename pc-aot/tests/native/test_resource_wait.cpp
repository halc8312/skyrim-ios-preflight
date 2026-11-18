// Authored concurrency cases around the actual pinned resource class.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>
#include <time.h>
static std::atomic<int> hookPhase{-1}, hookArrived{-1};
static std::atomic<bool> hookContinue{false};
void fixture_wait_hook(int phase) {
  if (hookPhase.load() != phase) return;
  hookArrived.store(phase);
  while (!hookContinue.load()) std::this_thread::yield();
}
// ACTUAL_RESOURCE
using dxvk::DxvkResource;
using dxvk::DxvkAccess;
using Clock = std::chrono::steady_clock;
template<typename F> void until(F predicate) {
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  while (!predicate()) {
    if (Clock::now() > deadline) { std::fputs("resource wait test deadline\n", stderr); std::abort(); }
    std::this_thread::yield();
  }
}
static void basic_access() {
  DxvkResource resource;
  resource.acquire(DxvkAccess::None); resource.release(DxvkAccess::None);
  assert(!resource.isInUse()); resource.waitIdle();
  resource.acquire(DxvkAccess::Read);
  assert(resource.isInUse() && !resource.isInUse(DxvkAccess::Write));
  resource.waitIdle(DxvkAccess::Write);
  resource.acquire(DxvkAccess::Write);
  std::atomic<int> started{0}; std::atomic<bool> readDone{false},writeDone{false};
  std::thread reader([&] {++started;resource.waitIdle(DxvkAccess::Read);readDone=true;});
  std::thread writer([&] {++started;resource.waitIdle(DxvkAccess::Write);writeDone=true;});
  until([&]{return started.load()==2;});
  resource.release(DxvkAccess::None);
  assert(!readDone.load() && !writeDone.load());
  resource.release(DxvkAccess::Write);
  until([&]{return writeDone.load();});assert(!readDone.load());
  resource.release(DxvkAccess::Read);
  until([&]{return readDone.load();});reader.join();writer.join();
}
static void boundary_wakes() {
#if PATCHED
  for(int phase=0;phase<2;++phase) {
    DxvkResource resource;resource.acquire(DxvkAccess::Write);
    hookArrived=-1;hookContinue=false;hookPhase=phase;
    std::atomic<bool> done{false},released{false};
    std::thread waiter([&]{resource.waitIdle();done=true;});
    until([&]{return hookArrived.load()==phase;});
    std::thread release([&]{resource.release(DxvkAccess::Write);released=true;});
    // At phase 0 no waiter is registered. At phase 1 the false predicate was
    // already read under the wait mutex, and the notifier must bridge sleep.
    until([&]{return !resource.isInUse();});
    if(phase==0) until([&]{return released.load();});
    hookContinue=true;
    until([&]{return done.load()&&released.load();});waiter.join();release.join();hookPhase=-1;
    assert(dxvk::g_iosResourceWaiters.load()==0);
  }
  // Two unrelated resource addresses in one bucket must never complete each
  // other's wait. Unrelated notifications are deliberately delivered too.
  std::vector<std::unique_ptr<DxvkResource>> resources;
  for(unsigned i=0;i<65;++i) resources.emplace_back(new DxvkResource);
  DxvkResource *a=nullptr,*b=nullptr;
  for(size_t i=0;i<resources.size()&&!a;++i) for(size_t j=i+1;j<resources.size();++j)
    if(&dxvk::resourceWaitBucket(resources[i].get())==&dxvk::resourceWaitBucket(resources[j].get())) {a=resources[i].get();b=resources[j].get();break;}
  assert(a&&b);a->acquire(DxvkAccess::Write);b->acquire(DxvkAccess::Write);
  std::atomic<bool> aDone{false},bDone{false};
  std::thread aw([&]{a->waitIdle();aDone=true;}),bw([&]{b->waitIdle();bDone=true;});
  until([]{return dxvk::g_iosResourceWaiters.load()==2;});
  dxvk::iosNotifyResourceWaiters(a);assert(!aDone.load()&&!bDone.load());
  a->release(DxvkAccess::Write);until([&]{return aDone.load();});assert(!bDone.load());
  b->release(DxvkAccess::Write);until([&]{return bDone.load();});aw.join();bw.join();
  assert(dxvk::g_iosResourceWaiters.load()==0);
#endif
}
static void visibility_and_reuse() {
  alignas(DxvkResource) unsigned char storage[sizeof(DxvkResource)];
  for(unsigned round=0;round<128;++round) {
    auto *resource=new(storage) DxvkResource;
    int values[2]={0,0};resource->acquire(DxvkAccess::Write);resource->acquire(DxvkAccess::Write);
    std::atomic<unsigned> started{0},done{0};std::thread waiters[4];
    for(auto& thread:waiters) thread=std::thread([&]{++started;resource->waitIdle();assert(values[0]==37&&values[1]==83);++done;});
    until([&]{return started.load()==4;});
    std::thread first([&]{values[0]=37;resource->release(DxvkAccess::Write);});
    std::thread second([&]{values[1]=83;resource->release(DxvkAccess::Write);});
    until([&]{return done.load()==4;});first.join();second.join();for(auto& thread:waiters)thread.join();
    resource->~DxvkResource();
  }
}
static uint64_t clock_ns(clockid_t id) {
  timespec ts;assert(clock_gettime(id,&ts)==0);return uint64_t(ts.tv_sec)*1000000000+ts.tv_nsec;
}
static void benchmark(int kind,unsigned count) {
  alignas(128) DxvkResource resource;
  alignas(128) DxvkResource occupied;
  std::thread concurrent;
  if(kind<0) {
    occupied.acquire(DxvkAccess::Write);
    std::atomic<bool> started{false};
    concurrent=std::thread([&]{started=true;occupied.waitIdle();});
    until([&]{return started.load();});
#if PATCHED
    until([]{return dxvk::g_iosResourceWaiters.load()==1;});
#endif
  }
  uint64_t start=clock_ns(CLOCK_MONOTONIC),cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID);
  uint64_t latency=0;
  if(kind<=0) for(unsigned i=0;i<count;++i) {resource.acquire(DxvkAccess::Write);resource.release(DxvkAccess::Write);resource.waitIdle();}
  else for(unsigned i=0;i<count;++i) {
    resource.acquire(DxvkAccess::Write);
    std::atomic<uint64_t> released{0};
    std::thread producer([&]{std::this_thread::sleep_for(std::chrono::microseconds(kind));released.store(clock_ns(CLOCK_MONOTONIC));resource.release(DxvkAccess::Write);});
    resource.waitIdle();uint64_t returned=clock_ns(CLOCK_MONOTONIC);producer.join();
    latency+=returned-released.load();
  }
  cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;start=clock_ns(CLOCK_MONOTONIC)-start;
  if(kind<0) {occupied.release(DxvkAccess::Write);concurrent.join();}
  std::printf("{\"kind_us\":%d,\"count\":%u,\"wall_ns\":%llu,\"waiter_cpu_ns\":%llu,\"return_delay_ns\":%llu}\n",kind,count,
              (unsigned long long)start,(unsigned long long)cpu,(unsigned long long)latency);
}
int main(int argc,char **argv) {
  if(argc==3) {benchmark(std::atoi(argv[1]),unsigned(std::strtoul(argv[2],nullptr,10)));return 0;}
  basic_access();boundary_wakes();visibility_and_reuse();
#if PATCHED
  SkyrimPerfWindow measured;skyrim_perf_take(SKYRIM_PERF_RESOURCE_WAIT,&measured);
  uint64_t samples=0;for(auto count:measured.buckets)samples+=count;
  assert(samples>=4 && measured.failures==0);
#endif
  std::printf("RESOURCE_WAIT access=PASS visibility=PASS reuse=128 boundaries=%d resource_bytes=%zu\n",PATCHED,sizeof(DxvkResource));
}

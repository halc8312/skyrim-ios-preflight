// Authored conversion / library boundaries around the extracted real cache
// method. This verifies observation placement, not Metal compiler performance.
#pragma once
#include "performance_metrics.h"

static uint64_t conversionSamples(const SkyrimPerfWindow& window) {
  uint64_t count=0;
  for(auto bucket:window.buckets)count+=bucket;
  return count;
}

static void drainConversionObservation() {
  SkyrimPerfWindow window;
  skyrim_perf_take(SKYRIM_PERF_MVK_CONVERT,&window);
  for(unsigned role=0;role<3;++role)
    skyrim_perf_context_take(SKYRIM_PERF_MVK_CONVERT,{role,0,0},&window);
}

template<class Request> static void conversionObservation(Request request) {
  drainConversionObservation();
  MVKPipelineCache cache;
  SkyrimPerfWindow window;
  {
    SkyrimPerfContextScope role(SKYRIM_PERF_ROLE_DRAW,SKYRIM_PERF_CONTEXT_KEEP);
    assert(request(cache,70,7000,false));
    skyrim_perf_take(SKYRIM_PERF_MVK_CONVERT,&window);
    assert(conversionSamples(window)==1 && window.failures==0);
    skyrim_perf_context_take(SKYRIM_PERF_MVK_CONVERT,{SKYRIM_PERF_ROLE_DRAW,0,0},&window);
    assert(conversionSamples(window)==1 && window.failures==0);
    assert(request(cache,70,7000,false)); // Ready hit does no conversion work.
    assert(!request(cache,70,7001,true)); // Compile-required refusal stays intact.
    skyrim_perf_take(SKYRIM_PERF_MVK_CONVERT,&window);
    assert(conversionSamples(window)==0);

    fixture.failConvert=1;
    assert(!request(cache,70,7001,false));
    skyrim_perf_take(SKYRIM_PERF_MVK_CONVERT,&window);
    assert(conversionSamples(window)==1 && window.failures==1);
    assert(request(cache,70,7001,false)); // Failed conversion still retries.
    skyrim_perf_take(SKYRIM_PERF_MVK_CONVERT,&window);
    assert(conversionSamples(window)==1 && window.failures==0);
  }
  drainConversionObservation();
  fixture.arm(7002,false);
  auto converting=std::async(std::launch::async,[&]{
    SkyrimPerfContextScope role(SKYRIM_PERF_ROLE_PREFETCH,SKYRIM_PERF_CONTEXT_KEEP);
    return request(cache,70,7002,false);
  });
  fixture.waitEntered();
  skyrim_perf_take(SKYRIM_PERF_MVK_CONVERT,&window);
  assert(conversionSamples(window)==0); // A still-running conversion is not complete.
  fixture.release();assert(converting.get());
  skyrim_perf_take(SKYRIM_PERF_MVK_CONVERT,&window);
  assert(conversionSamples(window)==1 && window.failures==0);
  skyrim_perf_context_take(SKYRIM_PERF_MVK_CONVERT,{SKYRIM_PERF_ROLE_PREFETCH,0,0},&window);
  assert(conversionSamples(window)==1);

  fixture.arm(7003,true);
  auto library=std::async(std::launch::async,[&]{
    SkyrimPerfContextScope role(SKYRIM_PERF_ROLE_PREFETCH,SKYRIM_PERF_CONTEXT_KEEP);
    return request(cache,70,7003,false);
  });
  fixture.waitEntered();
  // The conversion has already been recorded while library construction is
  // deliberately blocked. Completing the library cannot add another sample.
  skyrim_perf_take(SKYRIM_PERF_MVK_CONVERT,&window);
  assert(conversionSamples(window)==1 && window.failures==0);
  assert(library.wait_for(std::chrono::milliseconds(40))==std::future_status::timeout);
  fixture.release();assert(library.get());
  skyrim_perf_take(SKYRIM_PERF_MVK_CONVERT,&window);
  assert(conversionSamples(window)==0);
  skyrim_perf_context_take(SKYRIM_PERF_MVK_CONVERT,{SKYRIM_PERF_ROLE_PREFETCH,0,0},&window);
  assert(conversionSamples(window)==1);
  assert(skyrim_perf_context_get().role==SKYRIM_PERF_ROLE_OTHER);
  drainConversionObservation();
  puts("conversion_observation=PASS ready_hit_and_refusal_unmeasured=PASS conversion_failure_retry=PASS library_wait_excluded=PASS role_partition=PASS");
}

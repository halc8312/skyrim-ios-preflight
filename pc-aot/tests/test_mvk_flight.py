"""Compare current real macro lookup and an UNLINKED per-key flight prototype.

Only the Metal compiler boundary is authored. No real Apple scheduling proof.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import test_mvk_library as base

ROOT=base.ROOT

def prototype(source,converter,codec):
    old=base.adapter.LOCKED_LOOKUP
    try:
        base.adapter.LOCKED_LOOKUP=(ROOT/'experiments/mvk-flight/lookup.cpp.in').read_bytes().rstrip()
        cpp=base.fixture(source,converter,codec)
    finally:
        base.adapter.LOCKED_LOOKUP=old
    types=(ROOT/'experiments/mvk-flight/flight_types.h').read_text()
    cpp=cpp.replace('using namespace std;', '#include <functional>\n#include <stdexcept>\nstatic std::atomic<unsigned> testFollowers{0};\n'+types+'\nusing namespace std;',1)
    cpp=cpp.replace('flight->changed.wait(guard,', '++testFollowers;\n                        flight->changed.wait(guard,')
    cpp=cpp.replace('mutex _specializationLock;', 'mutex _specializationLock;\nmap<vector<pair<uint32_t,MVKShaderMacroValue>>, shared_ptr<PrototypeFlight>> _prototypeFlights;')
    cpp=cpp.replace('uint64_t blocked=0, fail=0;', 'function<void()> nested; uint64_t throwKey=0; bool nestedMayProceed=false;\n    uint64_t blocked=0, fail=0;')
    cpp=cpp.replace('if (val.value.ui64 == _owner->blocked)', 'if (val.value.ui64 == _owner->blocked && !(_owner->nestedMayProceed && prototypeCompileDepth>1))')
    cpp=cpp.replace('auto val=macros->front().second;', 'auto val=macros->front().second;\n        if (_owner->nested) {auto once=move(_owner->nested); _owner->nested={}; once();}')
    cpp=cpp.replace('_mtlLibrary = val.value.ui64', 'if (val.value.ui64==_owner->throwKey) throw runtime_error("authored compile exception");\n        _mtlLibrary = val.value.ui64')
    cpp=cpp.replace('owner.waitEntered(count);', 'owner.waitEntered(1); waitFollowers(7);')
    cpp=cpp.replace('constexpr unsigned count=8;', 'testFollowers=0; auto before=compiles.load();\n        constexpr unsigned count=8;')
    cpp=cpp.replace('static void concurrentCases()', '''static void waitFollowers(unsigned count) {
    auto deadline=chrono::steady_clock::now()+2s;
    while(testFollowers<count && chrono::steady_clock::now()<deadline) this_thread::yield();
    assert(testFollowers>=count);
}
static void concurrentCases()''')
    cpp=cpp.replace('assert(lib._specializationVariants.size()==3 && handles==4);', 'assert(lib._specializationVariants.size()==3 && handles==4);\n        assert(compiles==before+2 && lib._prototypeFlights.empty());')
    extra=r'''
static void exceptionalAndNestedCases() {
    MVKVulkanAPIDeviceObject owner(5);
    {
        MVKShaderLibrary lib(&owner); Input key{{7,55}};
        owner.nested=[&]{ assert(key.get(lib)->value==55); };
        auto before=compiles.load();
        auto* result=key.get(lib);
        assert(result->value==55 && compiles==before+2 && lib._prototypeFlights.empty());
        // Exceptions clean up admission. An explicit later call may retry;
        // null Metal results remain cached by the inherited failure test.
        owner.throwKey=66; Input throwing{{7,66}};
        bool caught=false;try {throwing.get(lib);}catch(const runtime_error&){caught=true;}
        assert(caught && lib._prototypeFlights.empty());
        owner.throwKey=0;assert(throwing.get(lib)->value==66);
        // Nested other key must not wait on a concurrent leader requiring
        // this callback to return. It compiles independently and publishes.
        owner.blocked=88;owner.nestedMayProceed=true; Input waiting{{7,88}};
        before=compiles.load();
        auto task=async(launch::async,[&]{return waiting.get(lib);});
        owner.waitEntered(1);
        owner.nested=[&]{assert(waiting.get(lib)->value==88);owner.allow();};
        Input parent{{7,99}};assert(parent.get(lib)->value==99);
        assert(task.get()->value==88 && lib._prototypeFlights.empty() && compiles==before+3);
    }
    assert(handles==0);
}
static void sharedExceptionCase() {
    MVKVulkanAPIDeviceObject owner(6);owner.blocked=66;owner.throwKey=66;
    {
        MVKShaderLibrary lib(&owner);testFollowers=0;auto before=compiles.load();
        vector<future<FixtureHandle*>> tasks;
        for(unsigned n=0;n<8;++n) tasks.push_back(async(launch::async,[&]{Input key{{7,66}};return key.get(lib);}));
        owner.waitEntered(1);waitFollowers(7);owner.allow();
        unsigned errors=0;for(auto& task:tasks)try{task.get();}catch(const runtime_error&){++errors;}
        assert(errors==8 && compiles==before+1 && lib._prototypeFlights.empty());
        owner.throwKey=0;Input key{{7,66}};assert(key.get(lib)->value==66);
    }
    assert(handles==0);
}
'''
    cpp=cpp.replace('int main() {',extra+'\nint main() {\n    exceptionalAndNestedCases();sharedExceptionCase();')
    cpp=cpp.replace('concurrent_hits=8000', 'concurrent_hits=8000 same_key_compile=8->1 production_enabled=false')
    return cpp

class MacroFlightTests(base.MoltenLibraryTests):
    # Retain inherited production tests, including its original 8 cold compiles.
    def test_unlinked_sharing_ownership_failures_and_nested_fallback(self):
        source,converter,codec=self.inputs()
        cxx=shutil.which(os.getenv('CXX','c++'))
        if not cxx:self.skipTest('C++ compiler required')
        with tempfile.TemporaryDirectory(prefix='macro-flight-') as tmp:
            tmp=Path(tmp); cpp=tmp/'flight.cpp'
            cpp.write_text(prototype(source,converter/'SPIRVToMSLConverter.h',codec))
            flags=['-std=c++17','-O2','-pthread','-Wall','-Wextra','-Werror','-Wno-unused-function']
            if os.getenv('SKYRIM_TEST_SANITIZERS')=='1':flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            subprocess.run([cxx,*flags,str(cpp),'-o',str(tmp/'flight')],check=True,timeout=60)
            result=subprocess.run([str(tmp/'flight')],capture_output=True,text=True,timeout=25)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('same_key_compile=8->1 production_enabled=false',result.stdout)
            print(result.stdout.strip())

if __name__=='__main__':unittest.main()

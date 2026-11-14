// Unlinked experiment. Waiting is NOT admitted by the production adapter.
struct PrototypeFlight {
    std::condition_variable changed;
    bool done=false;
    std::exception_ptr error;
};
// Never wait while a nested compile is active on this thread. A recursive
// request uses the old compile-and-publish route, preventing self/cross-key
// cycles introduced through synchronous nested compilation. This cannot
// detect caller-held external locks; production admission remains unresolved.
inline thread_local unsigned prototypeCompileDepth=0;
struct PrototypeCompileScope {
    PrototypeCompileScope() { ++prototypeCompileDepth; }
    ~PrototypeCompileScope() { --prototypeCompileDepth; }
};

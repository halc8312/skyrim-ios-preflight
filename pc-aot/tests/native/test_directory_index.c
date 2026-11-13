#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#define HOST_STAT_NSEC(st) ((st).st_mtimespec.tv_nsec)
#else
#define HOST_STAT_NSEC(st) ((st).st_mtim.tv_nsec)
#endif
static _Atomic uint64_t old_comparisons, new_comparisons;
static int old_compare(const char *a,const char *b){atomic_fetch_add(&old_comparisons,1);return strcasecmp(a,b);}
static int new_compare(const char *a,const char *b){atomic_fetch_add(&new_comparisons,1);return strcasecmp(a,b);}
static int fail_index;
static void *index_malloc(size_t size){if(fail_index){errno=ENOMEM;return NULL;}return malloc(size);}
/* ACTUAL_DIRECTORY_CACHE */
static uint64_t cases;
static void filename(char *name,unsigned n,int uniform) {
    int used=snprintf(name,260,"Asset-%05u-",n);
    unsigned pad=uniform?30:n%48;
    memset(name+used,'q',pad);strcpy(name+used+pad,".dds");
}
static void make_file(const char *dir,const char *name) {
    char path[2048];assert(snprintf(path,sizeof path,"%s/%s",dir,name)>0);
    int fd=open(path,O_CREAT|O_WRONLY,0600);assert(fd>=0);assert(!close(fd));
}
static void compare(const char *dir,const char *query,size_t cap) {
    char a[260],b[260];snprintf(a,sizeof a,"%s",query);memcpy(b,a,sizeof b);
    int x=old_dir_lookup_ci(dir,a,cap),y=new_dir_lookup_ci(dir,b,cap);
    assert(x==y&&!memcmp(a,b,sizeof a));++cases;
}
static void advance_mtime(const char *dir) {
    struct stat st;assert(!stat(dir,&st));
    struct timespec times[2]={{st.st_atime,0},{st.st_mtime+2,0}};
    assert(!utimensat(AT_FDCWD,dir,times,0));
}
static uint64_t ns(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static const char *parallel_dir;
static void *reader(void *arg){unsigned id=(unsigned)(uintptr_t)arg;for(unsigned i=0;i<1000;++i){char a[260],b[260];filename(a,(i+id)%256,0);a[0]='a';strcpy(b,a);assert(old_dir_lookup_ci(parallel_dir,a,sizeof a)==new_dir_lookup_ci(parallel_dir,b,sizeof b));assert(!strcmp(a,b));}return NULL;}
static void cleanup(void) {
    for(unsigned i=0;i<16;++i){free(old_g_dircache[i].names);free(new_g_dircache[i].names);free(new_g_dircache[i].index);}
}
int main(int argc,char **argv) {
    assert(argc>=2);const char *dir=argv[1];char name[260];
    const unsigned count=argc>3?(unsigned)strtoul(argv[3],NULL,10):512;
    const int uniform=argc>4?atoi(argv[4]):0;
    for(unsigned i=0;i<count;++i){filename(name,i,uniform);make_file(dir,name);}
    if(argc>2) {
        int (*lookup)(const char *,char *,size_t)=argv[2][0]=='o'?old_dir_lookup_ci:new_dir_lookup_ci;
        strcpy(name,"absent.dds");uint64_t cold=ns();lookup(dir,name,sizeof name);cold=ns()-cold;
        const unsigned rounds=20000;unsigned hits=0;uint64_t start=ns();
        for(unsigned i=0;i<rounds;++i){
            if(i&1){filename(name,i%count,uniform);name[0]='a';}
            else strcpy(name,uniform?"Asset-99999-qqqqqqqqqqqqqqqqqqqqqqqqqqqqqq.dds":"not-here.dds");
            hits+=lookup(dir,name,sizeof name);
        }
        printf("{\"ns\":%.6f,\"cold_ns\":%" PRIu64 ",\"hits\":%u}\n",(double)(ns()-start)/rounds,cold,hits);
        cleanup();return 0;
    }
    make_file(dir,"MiXeD.txt");make_file(dir,"A-name");make_file(dir,"a-NAME");
    make_file(dir,"日本語.dds");make_file(dir,"\xc3\x84-file");
    for(unsigned i=0;i<count;++i){filename(name,i,0);name[0]='a';compare(dir,name,sizeof name);}
    for(unsigned cap=0;cap<=12;++cap){compare(dir,"mixed.TXT",cap);compare(dir,"never-existed",cap);}
    compare(dir,"A-NAME",sizeof name);compare(dir,"日本語.dds",sizeof name);compare(dir,"\xc3\x84-file",sizeof name);
    compare("/does-not-exist-authored-fixture", "missing",sizeof name);
    compare(dir,"NewArrival",sizeof name);make_file(dir,"NewArrival");advance_mtime(dir);compare(dir,"newarrival",sizeof name);
    make_file(dir,"SecondArrival");old_dircache_forget(dir);new_dircache_forget(dir);compare(dir,"secondarrival",sizeof name);
    // A failed optional index allocation must still return the original results.
    new_dircache_forget(dir);fail_index=1;errno=EDOM;compare(dir,"mixed.txt",sizeof name);fail_index=0;
    new_dircache_forget(dir);compare(dir,"mixed.txt",sizeof name);
    parallel_dir=dir;pthread_t threads[4];for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],NULL,reader,(void *)(uintptr_t)i));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],NULL));
    // Both caches rotate their existing 16 slots; a new index belongs to its listing.
    for(unsigned i=0;i<20;++i){char sub[2048];snprintf(sub,sizeof sub,"%s/child-%u",dir,i);assert(!mkdir(sub,0700));make_file(sub,"Small");compare(sub,"small",sizeof name);}
    compare(dir,"mixed.txt",sizeof name);
#if MEASURED
    old_comparisons=new_comparisons=0;
    for(unsigned i=0;i<100;++i)compare(dir,"not-here.dds",sizeof name);
    assert(old_comparisons>50000 && new_comparisons<old_comparisons/8);
#endif
    // Synthetic same-length case variants retain their first listing order.
    char synthetic[1024]="Aa\0aA\0bb\0";
    size_t synthetic_len=9;
    for(unsigned n=0;n<40;++n)synthetic_len+=(size_t)snprintf(synthetic+synthetic_len,sizeof synthetic-synthetic_len,"other-%u",n)+1;
    new_DirCache small={0};small.names=synthetic;small.len=synthetic_len;
    dir_index_build(&small);assert(small.index&&small.index_count==43);
    assert(dir_index_find(&small,"aa")==synthetic);
    free(small.index);small.index=NULL;small.index_count=0;
    fail_index=1;errno=EDOM;dir_index_build(&small);assert(!small.index&&errno==EDOM);fail_index=0;
    new_DirCache bounded={0};bounded.names=synthetic;bounded.len=(size_t)UINT32_MAX+1;
    dir_index_build(&bounded);assert(!bounded.index);
    char *many=malloc(65537*2);assert(many);
    for(unsigned n=0;n<65537;++n){many[n*2]='x';many[n*2+1]=0;}
    bounded.names=many;bounded.len=65537*2;dir_index_build(&bounded);assert(!bounded.index);free(many);
    printf("DIRECTORY_INDEX passed cases=%" PRIu64 " parallel=4000 comparisons=%" PRIu64 "->%" PRIu64 "\n",cases,(uint64_t)old_comparisons,(uint64_t)new_comparisons);
    cleanup();
}

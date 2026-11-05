// Stream-ordered allocator contention: T threads per GPU on G GPUs, each looping cudaMallocFromPoolAsync/cudaFreeAsync
// (+ optional tiny kernel + cudaStreamSynchronize every K iters) on its own stream. Reports per-call latency and aggregate rate.
// usage: alloc <gpus e.g. 0,1,2,3> <threads_per_gpu> [iters=20000] [sync_every=8]
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>
#define CK(x) do{cudaError_t e=(x); if(e!=cudaSuccess){fprintf(stderr,"%s:%d %s\n",__FILE__,__LINE__,cudaGetErrorString(e));exit(1);}}while(0)
__global__ void touch(char* p){ if(threadIdx.x==0) p[0]=1; }
int main(int argc,char**argv){
  std::vector<int> g; {char*s=strdup(argv[1]);for(char*t=strtok(s,",");t;t=strtok(nullptr,","))g.push_back(atoi(t));}
  int T=atoi(argv[2]); int iters=argc>3?atoi(argv[3]):20000; int K=argc>4?atoi(argv[4]):8; int grant=argc>5?atoi(argv[5]):0;
  std::vector<cudaMemPool_t> pools(g.size());
  for(size_t i=0;i<g.size();i++){CK(cudaSetDevice(g[i])); cudaMemPoolProps p{}; p.allocType=cudaMemAllocationTypePinned; p.location.type=cudaMemLocationTypeDevice; p.location.id=g[i];
    CK(cudaMemPoolCreate(&pools[i],&p)); uint64_t th=~0ull; CK(cudaMemPoolSetAttribute(pools[i],cudaMemPoolAttrReleaseThreshold,&th));}
  if(grant){ for(size_t i=0;i<g.size();i++) for(size_t j=0;j<g.size();j++) if(i!=j){ int can=0; CK(cudaDeviceCanAccessPeer(&can,g[j],g[i])); if(!can) continue;
      cudaMemAccessDesc d{}; d.location.type=cudaMemLocationTypeDevice; d.location.id=g[j]; d.flags=cudaMemAccessFlagsProtReadWrite; CK(cudaMemPoolSetAccess(pools[i],&d,1)); } }
  size_t sizes[]={256,4096,65536,1<<20,8<<20,64<<20};
  std::vector<std::vector<double>> lat(g.size()*T);
  auto work=[&](int gi,int ti){ CK(cudaSetDevice(g[gi])); cudaStream_t s; CK(cudaStreamCreateWithFlags(&s,cudaStreamNonBlocking));
    auto& L=lat[gi*T+ti]; L.reserve(iters*2); std::vector<void*> live;
    for(int i=0;i<iters;i++){ size_t sz=sizes[(i*7+ti)%6]; void* p;
      auto a=std::chrono::steady_clock::now(); CK(cudaMallocFromPoolAsync(&p,sz,pools[gi],s)); auto b=std::chrono::steady_clock::now();
      L.push_back(std::chrono::duration<double,std::micro>(b-a).count());
      touch<<<1,32,0,s>>>((char*)p); live.push_back(p);
      if(live.size()>=4){ for(void* q:live){auto c=std::chrono::steady_clock::now(); CK(cudaFreeAsync(q,s)); auto d=std::chrono::steady_clock::now(); L.push_back(std::chrono::duration<double,std::micro>(d-c).count());} live.clear();}
      if(K && i%K==0) CK(cudaStreamSynchronize(s)); }
    for(void* q:live) CK(cudaFreeAsync(q,s)); CK(cudaStreamSynchronize(s)); };
  auto t0=std::chrono::steady_clock::now();
  {std::vector<std::thread> th; for(size_t gi=0;gi<g.size();gi++) for(int ti=0;ti<T;ti++) th.emplace_back(work,gi,ti); for(auto&x:th) x.join();}
  double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
  std::vector<double> all; for(auto&L:lat) all.insert(all.end(),L.begin(),L.end()); std::sort(all.begin(),all.end());
  size_t n=all.size(); double sum=0; for(double x:all) sum+=x;
  printf("grant=%d gpus=%s thr/gpu=%d calls=%zu wall=%.2fs rate=%.0f calls/s per-thread-rate=%.0f mean=%.1fus p50=%.1fus p90=%.1fus p99=%.1fus\n",
    grant,argv[1],T,n,sec,n/sec,n/sec/(g.size()*T),sum/n,all[n/2],all[n*9/10],all[n*99/100]);
}

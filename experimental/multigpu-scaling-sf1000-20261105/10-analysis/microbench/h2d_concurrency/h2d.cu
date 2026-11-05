// Concurrent pinned-host -> device (and D2H, and P2P) bandwidth across a GPU subset.
// usage: h2d <h2d|d2h|bidir> <gpu list e.g. 0,1,2,3> [MiB per copy=1024] [reps=10]
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <string>
#define CK(x) do{cudaError_t e=(x); if(e!=cudaSuccess){fprintf(stderr,"%s:%d %s\n",__FILE__,__LINE__,cudaGetErrorString(e));exit(1);}}while(0)
int main(int argc,char**argv){
  std::string mode=argv[1]; std::vector<int> gpus; {char*s=strdup(argv[2]);for(char*t=strtok(s,",");t;t=strtok(nullptr,","))gpus.push_back(atoi(t));}
  size_t mib = argc>3?atol(argv[3]):1024; int reps=argc>4?atoi(argv[4]):10; size_t bytes=mib<<20;
  int n=gpus.size(); std::vector<void*> h(n), d(n), h2(n), d2(n); std::vector<cudaStream_t> s(n), s2(n);
  for(int i=0;i<n;i++){CK(cudaSetDevice(gpus[i])); CK(cudaMallocHost(&h[i],bytes)); memset(h[i],1,bytes); CK(cudaMalloc(&d[i],bytes)); CK(cudaStreamCreateWithFlags(&s[i],cudaStreamNonBlocking));
    CK(cudaMallocHost(&h2[i],bytes)); CK(cudaMalloc(&d2[i],bytes)); CK(cudaStreamCreateWithFlags(&s2[i],cudaStreamNonBlocking));}
  auto run=[&](int i){ CK(cudaSetDevice(gpus[i])); for(int r=0;r<reps;r++){
      if(mode!="d2h") CK(cudaMemcpyAsync(d[i],h[i],bytes,cudaMemcpyHostToDevice,s[i]));
      if(mode!="h2d") CK(cudaMemcpyAsync(h2[i],d2[i],bytes,cudaMemcpyDeviceToHost,s2[i]));}
    CK(cudaStreamSynchronize(s[i])); CK(cudaStreamSynchronize(s2[i]));};
  // warmup
  {std::vector<std::thread> t; for(int i=0;i<n;i++) t.emplace_back(run,i); for(auto&x:t) x.join();}
  auto t0=std::chrono::steady_clock::now();
  {std::vector<std::thread> t; for(int i=0;i<n;i++) t.emplace_back(run,i); for(auto&x:t) x.join();}
  double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
  double dirs = (mode=="bidir")?2:1;
  double tot = dirs*n*reps*(double)bytes/1e9/sec;
  printf("mode=%s gpus=%s aggregate=%.1f GB/s per_gpu=%.1f GB/s\n",mode.c_str(),argv[2],tot,tot/n);
}

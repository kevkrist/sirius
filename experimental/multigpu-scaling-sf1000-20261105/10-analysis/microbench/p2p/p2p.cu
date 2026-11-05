// P2P bandwidth: pairs list "s>d,s>d,..." run concurrently; optional concurrent H2D on given gpus.
// usage: p2p <pairs> <peer_access 0|1> [h2d_gpus e.g. 0,1,2,3 or -] [MiB=512] [reps=10]
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
  std::vector<std::pair<int,int>> pairs; {char*s=strdup(argv[1]);for(char*t=strtok(s,",");t;t=strtok(nullptr,",")){int a,b;sscanf(t,"%d>%d",&a,&b);pairs.push_back({a,b});}}
  int peer=atoi(argv[2]); std::vector<int> hg; if(argc>3&&strcmp(argv[3],"-")){char*s=strdup(argv[3]);for(char*t=strtok(s,",");t;t=strtok(nullptr,","))hg.push_back(atoi(t));}
  size_t bytes=(size_t)(argc>4?atol(argv[4]):512)<<20; int reps=argc>5?atoi(argv[5]):10;
  int ng; CK(cudaGetDeviceCount(&ng));
  if(peer) for(auto&p:pairs){int can; CK(cudaDeviceCanAccessPeer(&can,p.second,p.first)); if(!can){printf("no peer %d<-%d\n",p.second,p.first);continue;}
     CK(cudaSetDevice(p.second)); cudaError_t e=cudaDeviceEnablePeerAccess(p.first,0); if(e!=cudaErrorPeerAccessAlreadyEnabled) CK(e); cudaGetLastError();
     CK(cudaSetDevice(p.first)); e=cudaDeviceEnablePeerAccess(p.second,0); if(e!=cudaErrorPeerAccessAlreadyEnabled) CK(e); cudaGetLastError();}
  int n=pairs.size(); std::vector<void*> src(n),dst(n); std::vector<cudaStream_t> st(n);
  for(int i=0;i<n;i++){CK(cudaSetDevice(pairs[i].first));CK(cudaMalloc(&src[i],bytes));CK(cudaSetDevice(pairs[i].second));CK(cudaMalloc(&dst[i],bytes));CK(cudaStreamCreateWithFlags(&st[i],cudaStreamNonBlocking));}
  int m=hg.size(); std::vector<void*> hh(m),hd(m); std::vector<cudaStream_t> hs(m);
  for(int i=0;i<m;i++){CK(cudaSetDevice(hg[i]));CK(cudaMallocHost(&hh[i],bytes));CK(cudaMalloc(&hd[i],bytes));CK(cudaStreamCreateWithFlags(&hs[i],cudaStreamNonBlocking));}
  double p2p_sec=0,h2d_sec=0;
  auto go=[&](bool timed){
    std::vector<std::thread> t; std::vector<double> ps(n),hs_(m);
    for(int i=0;i<n;i++) t.emplace_back([&,i]{CK(cudaSetDevice(pairs[i].second)); auto t0=std::chrono::steady_clock::now();
      for(int r=0;r<reps;r++) CK(cudaMemcpyPeerAsync(dst[i],pairs[i].second,src[i],pairs[i].first,bytes,st[i])); CK(cudaStreamSynchronize(st[i]));
      ps[i]=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();});
    for(int i=0;i<m;i++) t.emplace_back([&,i]{CK(cudaSetDevice(hg[i])); auto t0=std::chrono::steady_clock::now();
      for(int r=0;r<reps;r++) CK(cudaMemcpyAsync(hd[i],hh[i],bytes,cudaMemcpyHostToDevice,hs[i])); CK(cudaStreamSynchronize(hs[i]));
      hs_[i]=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();});
    for(auto&x:t) x.join();
    if(timed){ double pt=0,ht=0; for(int i=0;i<n;i++) pt+=reps*(double)bytes/1e9/ps[i]; for(int i=0;i<m;i++) ht+=reps*(double)bytes/1e9/hs_[i];
      printf("pairs=%s peer=%d h2d=%s  p2p_aggregate=%.1f GB/s (per-pair %.1f)  h2d_aggregate=%.1f GB/s\n",argv[1],peer,argc>3?argv[3]:"-",pt,n?pt/n:0,ht);}
  };
  go(false); go(true);
}

// d2h_foreign: D2H of src-device memory into 1 MiB pinned blocks, issued on (a) a src-device stream vs
// (b) a dst-device stream under cudaSetDevice(src) (the peer_copy_into pattern), with K concurrent streams.
// Also the full staged clone: serial legs (D2H all, sync, H2D all, sync) vs overlapped (H2D chunk waits on D2H chunk event).
#include <cuda_runtime.h>
#include <cstdio>
#include <vector>
#include <chrono>
#define CK(x) do{cudaError_t e=(x); if(e!=cudaSuccess){printf("ERR %s @%d: %s\n",#x,__LINE__,cudaGetErrorString(e)); exit(1);} }while(0)
using clk=std::chrono::steady_clock;
static const size_t CH=1<<20;
int main(int argc,char**argv){
  int src=atoi(argv[1]), dst=atoi(argv[2]); size_t MB=argc>3?atoi(argv[3]):256; int reps=5;
  size_t bytes=MB*CH; int nch=MB;
  void *dsrc,*ddst; CK(cudaSetDevice(src)); CK(cudaMalloc(&dsrc,bytes)); CK(cudaMemset(dsrc,1,bytes));
  CK(cudaSetDevice(dst)); CK(cudaMalloc(&ddst,bytes));
  std::vector<void*> blk(nch); for(int i=0;i<nch;i++) CK(cudaHostAlloc(&blk[i],CH,cudaHostAllocPortable));
  for(int K: {1,4}){
    std::vector<cudaStream_t> ss(K), sd(K);
    CK(cudaSetDevice(src)); for(auto&s:ss) CK(cudaStreamCreateWithFlags(&s,cudaStreamNonBlocking));
    CK(cudaSetDevice(dst)); for(auto&s:sd) CK(cudaStreamCreateWithFlags(&s,cudaStreamNonBlocking));
    for(int mode=0;mode<2;mode++){ // 0: src-device stream, 1: dst-device stream (foreign)
      double best=1e9;
      for(int r=0;r<reps;r++){
        CK(cudaSetDevice(src)); CK(cudaDeviceSynchronize()); CK(cudaSetDevice(dst)); CK(cudaDeviceSynchronize());
        CK(cudaSetDevice(src));
        auto t0=clk::now();
        for(int i=0;i<nch;i++){ cudaStream_t s= mode==0? ss[i%K] : sd[i%K];
          CK(cudaMemcpyAsync(blk[i],(char*)dsrc+i*CH,CH,cudaMemcpyDeviceToHost,s)); }
        for(int k=0;k<K;k++){ CK(cudaStreamSynchronize(ss[k])); CK(cudaStreamSynchronize(sd[k])); }
        double dt=std::chrono::duration<double>(clk::now()-t0).count(); if(dt<best) best=dt;
      }
      printf("src %d dst %d K=%d D2H 1MiB chunks on %s stream: %.1f GB/s\n",src,dst,K,mode?"DST(foreign)":"SRC(own)",bytes/best/1e9);
    }
    // H2D baseline on dst stream
    { double best=1e9; for(int r=0;r<reps;r++){ CK(cudaSetDevice(dst)); CK(cudaDeviceSynchronize()); auto t0=clk::now();
        for(int i=0;i<nch;i++) CK(cudaMemcpyAsync((char*)ddst+i*CH,blk[i],CH,cudaMemcpyHostToDevice,sd[i%K]));
        for(int k=0;k<K;k++) CK(cudaStreamSynchronize(sd[k]));
        double dt=std::chrono::duration<double>(clk::now()-t0).count(); if(dt<best) best=dt; }
      printf("src %d dst %d K=%d H2D 1MiB chunks on dst stream: %.1f GB/s\n",src,dst,K,bytes/best/1e9); }
  }
  // full clone patterns, 20 MiB clone x 12 buffers? use clone = whole buffer (MB MiB) as one leaf buffer
  cudaStream_t sdst, ssrc; CK(cudaSetDevice(dst)); CK(cudaStreamCreateWithFlags(&sdst,cudaStreamNonBlocking));
  CK(cudaSetDevice(src)); CK(cudaStreamCreateWithFlags(&ssrc,cudaStreamNonBlocking));
  std::vector<cudaEvent_t> ev(nch); CK(cudaSetDevice(src)); for(auto&evx:ev) CK(cudaEventCreateWithFlags(&evx,cudaEventDisableTiming));
  for(int pat=0;pat<4;pat++){
    double best=1e9; double waitsum=0;
    for(int r=0;r<reps;r++){
      CK(cudaSetDevice(src)); CK(cudaDeviceSynchronize()); CK(cudaSetDevice(dst)); CK(cudaDeviceSynchronize());
      auto t0=clk::now();
      if(pat==0||pat==1){ // serial legs (as peer_copy_into): D2H on (0) dst stream foreign / (1) src stream, sync, H2D on dst, sync
        cudaStream_t s1= pat==0? sdst: ssrc; CK(cudaSetDevice(src));
        for(int i=0;i<nch;i++) CK(cudaMemcpyAsync(blk[i],(char*)dsrc+i*CH,CH,cudaMemcpyDeviceToHost,s1));
        CK(cudaStreamSynchronize(s1)); CK(cudaSetDevice(dst));
        for(int i=0;i<nch;i++) CK(cudaMemcpyAsync((char*)ddst+i*CH,blk[i],CH,cudaMemcpyHostToDevice,sdst));
        CK(cudaStreamSynchronize(sdst));
      } else if(pat==2) { // overlapped A: per chunk D2H(src stream)+event, dst waits event; then all H2D
        for(int i=0;i<nch;i++){ CK(cudaSetDevice(src)); CK(cudaMemcpyAsync(blk[i],(char*)dsrc+i*CH,CH,cudaMemcpyDeviceToHost,ssrc));
          CK(cudaEventRecord(ev[i],ssrc)); CK(cudaSetDevice(dst)); auto w0=clk::now(); CK(cudaStreamWaitEvent(sdst,ev[i],0)); waitsum+=std::chrono::duration<double>(clk::now()-w0).count(); }
        CK(cudaSetDevice(dst));
        for(int i=0;i<nch;i++) CK(cudaMemcpyAsync((char*)ddst+i*CH,blk[i],CH,cudaMemcpyHostToDevice,sdst));
        CK(cudaStreamSynchronize(sdst));
      } else { // overlapped B: issue all D2H+events first, then interleave wait(ev i) + H2D i
        CK(cudaSetDevice(src));
        for(int i=0;i<nch;i++){ CK(cudaMemcpyAsync(blk[i],(char*)dsrc+i*CH,CH,cudaMemcpyDeviceToHost,ssrc)); CK(cudaEventRecord(ev[i],ssrc)); }
        CK(cudaSetDevice(dst));
        for(int i=0;i<nch;i++){ auto w0=clk::now(); CK(cudaStreamWaitEvent(sdst,ev[i],0)); waitsum+=std::chrono::duration<double>(clk::now()-w0).count();
          CK(cudaMemcpyAsync((char*)ddst+i*CH,blk[i],CH,cudaMemcpyHostToDevice,sdst)); }
        CK(cudaStreamSynchronize(sdst));
      }
      double dt=std::chrono::duration<double>(clk::now()-t0).count(); if(dt<best) best=dt;
    }
    const char* nm[]={"serial legs, D2H on DST(foreign) stream [current code]","serial legs, D2H on SRC stream","overlapped legs, D2H on SRC stream + events","overlapped B: all D2H+events, then wait_i+H2D_i"};
    printf("src %d dst %d clone %zu MiB: %-55s %.2f ms = %.1f GB/s (StreamWaitEvent host time/rep %.2f ms)\n",src,dst,MB,nm[pat],best*1e3,bytes/best/1e9,waitsum/reps*1e3);
  }
  return 0;
}

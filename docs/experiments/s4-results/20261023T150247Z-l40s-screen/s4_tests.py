import subprocess,sys
for schedule in ('current_direct','concurrent_ingress'):
 for label,exe,args in [('focused','sirius_unittest',['[s4_publication]','--order','decl']),('fault','sirius_cuda_fault_test',[])]:
  cmd=[sys.executable,'/tmp/s4_capture.py',schedule+'-'+label,'pixi','run','--as-is','--manifest-path','/home/ubuntu/sirius/pixi.toml','build/s4-retry-xxhash/'+schedule+'/'+exe,*args]
  p=subprocess.run(cmd)
  if p.returncode: sys.exit(p.returncode)

# S0 hardware

Recorded 2026-11-13T21:07:15+00:00 on ip-172-31-42-26.

## GPUs
```
GPU 0: NVIDIA RTX PRO 6000 Blackwell Server Edition (UUID: GPU-1d7a870c-b324-ee92-0e37-ae18bcb120b9)
GPU 1: NVIDIA RTX PRO 6000 Blackwell Server Edition (UUID: GPU-64d5a7e7-3dbe-17bf-6b21-ab383f725ff9)
GPU 2: NVIDIA RTX PRO 6000 Blackwell Server Edition (UUID: GPU-e11d1e28-3710-dbc8-0391-0717d393da03)
GPU 3: NVIDIA RTX PRO 6000 Blackwell Server Edition (UUID: GPU-93e4f8ae-f0a8-b3d2-67b7-f43f9a972c06)
index, name, memory.total [MiB], pci.bus_id, pcie.link.gen.max, pcie.link.width.max, clocks.max.sm [MHz], persistence_mode, driver_version
0, NVIDIA RTX PRO 6000 Blackwell Server Edition, 97887 MiB, 00000000:52:00.0, 5, 16, 2430 MHz, Enabled, 595.112.07
1, NVIDIA RTX PRO 6000 Blackwell Server Edition, 97887 MiB, 00000000:53:00.0, 5, 16, 2430 MHz, Enabled, 595.112.07
2, NVIDIA RTX PRO 6000 Blackwell Server Edition, 97887 MiB, 00000000:63:00.0, 5, 16, 2430 MHz, Enabled, 595.112.07
3, NVIDIA RTX PRO 6000 Blackwell Server Edition, 97887 MiB, 00000000:64:00.0, 5, 16, 2430 MHz, Enabled, 595.112.07
| NVIDIA-SMI 595.112.07             Driver Version: 595.112.07     CUDA Version: 13.2     |
+-----------------------------------------+------------------------+----------------------+
```

## PCIe topology (`nvidia-smi topo -m`)
```
	[4mGPU0	GPU1	GPU2	GPU3	CPU Affinity	NUMA Affinity	GPU NUMA ID[0m
GPU0	 X 	PIX	NODE	NODE	0-95	0		N/A
GPU1	PIX	 X 	NODE	NODE	0-95	0		N/A
GPU2	NODE	NODE	 X 	PIX	0-95	0		N/A
GPU3	NODE	NODE	PIX	 X 	0-95	0		N/A

Legend:

  X    = Self
  SYS  = Connection traversing PCIe as well as the SMP interconnect between NUMA nodes (e.g., QPI/UPI)
  NODE = Connection traversing PCIe as well as the interconnect between PCIe Host Bridges within a NUMA node
  PHB  = Connection traversing PCIe as well as a PCIe Host Bridge (typically the CPU)
  PXB  = Connection traversing multiple PCIe bridges (without traversing the PCIe Host Bridge)
  PIX  = Connection traversing at most a single PCIe bridge
  NV#  = Connection traversing a bonded set of # NVLinks
```

## P2P read / write capability (`nvidia-smi topo -p2p r|w`)
```
 	[4mGPU0	GPU1	GPU2	GPU3	[0m
 GPU0	X	OK	NS	NS	
 GPU1	OK	X	NS	NS	
 GPU2	NS	NS	X	OK	
 GPU3	NS	NS	OK	X	

Legend:

  X    = Self
  OK   = Status Ok
  CNS  = Chipset not supported
  GNS  = GPU not supported
  TNS  = Topology not supported
  NS   = Not supported
  U    = Unknown
 	[4mGPU0	GPU1	GPU2	GPU3	[0m
 GPU0	X	OK	NS	NS	
 GPU1	OK	X	NS	NS	
 GPU2	NS	NS	X	OK	
 GPU3	NS	NS	OK	X	

Legend:

  X    = Self
  OK   = Status Ok
  CNS  = Chipset not supported
  GNS  = GPU not supported
  TNS  = Topology not supported
  NS   = Not supported
  U    = Unknown
```

## Host
```
Linux 7.0.0-1014-aws x86_64
96
               total        used        free      shared  buff/cache   available
Mem:             999          16         977           0          11         983
Swap:              0           0           0
Model name:                              Intel(R) Xeon(R) Platinum 8559C
Socket(s):                               1
NUMA node(s):                            1
NUMA node0 CPU(s):                       0-95
available: 1 nodes (0)
node 0 cpus: 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63 64 65 66 67 68 69 70 71 72 73 74 75 76 77 78 79 80 81 82 83 84 85 86 87 88 89 90 91 92 93 94 95
node 0 size: 1023524 MB
node 0 free: 1000824 MB
node distances:
node   0 
  0:  10 
Filesystem                      Size  Used Avail Use% Mounted on
/dev/mapper/vg.01-lv_ephemeral  6.9T  394M  6.6T   1% /opt/dlami/nvme
/dev/mapper/vg.01-lv_ephemeral  6.9T  394M  6.6T   1% /opt/dlami/nvme
```

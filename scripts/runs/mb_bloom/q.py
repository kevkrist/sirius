import sqlite3,sys
c=sqlite3.connect(sys.argv[1])
t=[r[0] for r in c.execute("select name from sqlite_master where type='table'")]
print([x for x in t if 'KERNEL' in x or 'RUNTIME' in x or 'NVTX' in x or 'MEMSET' in x])
print('-- kernels')
for r in c.execute("""select substr(s.value,1,90), count(*), round(sum(k.end-k.start)/1e6,3), round(avg(k.end-k.start)/1e6,4), round(min(k.end-k.start)/1e6,4), round(max(k.end-k.start)/1e6,4), min(k.gridX), min(k.blockX)
 from CUPTI_ACTIVITY_KIND_KERNEL k join StringIds s on s.id=k.shortName group by 1 order by 3 desc limit 12"""): print(r)
print('-- api')
for r in c.execute("""select s.value, count(*), round(sum(r.end-r.start)/1e6,3), round(avg(r.end-r.start)/1e3,2) from CUPTI_ACTIVITY_KIND_RUNTIME r join StringIds s on s.id=r.nameId group by 1 order by 3 desc limit 14"""): print(r)
try:
  print('-- nvtx')
  for r in c.execute("""select coalesce(n.text,s.value), count(*), round(sum(n.end-n.start)/1e6,3), round(avg(n.end-n.start)/1e3,2) from NVTX_EVENTS n left join StringIds s on s.id=n.textId group by 1 order by 3 desc limit 8"""): print(r)
except Exception as e: print(e)

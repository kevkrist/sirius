import sqlite3,sys
db,s,e=sys.argv[1],int(sys.argv[2]),int(sys.argv[3])
c=sqlite3.connect(db)
ids={r[1]:r[0] for r in c.execute("select id,value from StringIds where value like 'convert:gpu_to_gpu%'")}
print(ids.keys())
for name,i in ids.items():
    rows=c.execute("select start,end,globalTid from NVTX_EVENTS where (textId=? or text=?) and start>=? and end<=?",(i,name,s,e)).fetchall()
    n=len(rows); w=sum(r[1]-r[0] for r in rows)/1e6
    print(name,n,'%.0f ms'%w)
# memcpy bytes by kind in window per copyKind
for r in c.execute("select copyKind,srcKind,dstKind,count(*),sum(bytes)/1e9,sum(end-start)/1e6 from CUPTI_ACTIVITY_KIND_MEMCPY where start>=? and end<=? group by 1,2,3 order by 5 desc",(s,e)):
    print(r)

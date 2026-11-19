import sqlite3, sys, re
db = sys.argv[1]; filt = sys.argv[2] if len(sys.argv) > 2 else None
c = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
rows = c.execute("""select s.value, count(*), sum(k.end-k.start)/1e6 from CUPTI_ACTIVITY_KIND_KERNEL k
 join StringIds s on s.id=k.demangledName group by s.value order by 3 desc""").fetchall()
tot = sum(r[2] for r in rows)
print("total kernel GPU ms (all 3 iters)", round(tot,1))
n=0
for name, cnt, ms in rows:
    if filt and not re.search(filt, name, re.I): continue
    print(f"{ms:9.2f} ms  n={cnt:6d}  {name[:230]}")
    n+=1
    if n>=int(sys.argv[3]) if len(sys.argv)>3 else n>=25: break

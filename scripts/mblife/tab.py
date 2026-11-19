import sys,re
# usage: tab.py file case col1,col2,... [filter k=v ...]
f,case,cols=sys.argv[1],sys.argv[2],sys.argv[3].split(',')
flt=dict(a.split('=') for a in sys.argv[4:])
print(' '.join(cols))
for l in open(f):
    if not l.startswith('ROW,'+case+','): continue
    d=dict(x.split('=') for x in l.strip().split(',')[2:])
    if all(d.get(k)==v for k,v in flt.items()):
        print(' '.join(d.get(c,'-') for c in cols))

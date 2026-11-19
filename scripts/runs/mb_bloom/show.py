import sys
cols=sys.argv[2].split(',')
for l in open(sys.argv[1]):
    if not l.startswith('RES'): print(l.strip()[:150]); continue
    d=dict(x.split('=') for x in l.strip().split(',')[2:])
    print(' '.join((c+'='+d.get(c,'?')).ljust(len(c)+10) for c in cols))

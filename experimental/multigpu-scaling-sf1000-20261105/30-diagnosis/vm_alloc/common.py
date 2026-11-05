import csv
B='/home/ubuntu/sirius-scaling-20261105/'
def tim(d):
    r={}
    for row in csv.DictReader(open(B+d+'/timings.csv')):
        try: q=int(row['query'])
        except: continue
        r.setdefault(q,{})[int(row['iter'])]=float(row['seconds'])
    return r
def hot(d):
    t=tim(d); return {q:sum(v[i] for i in (2,3,4) if i in v)/len([i for i in (2,3,4) if i in v]) for q,v in t.items()}
def mp(d):
    r={}
    for row in csv.DictReader(open(B+d+'/nsys/map.csv')):
        if row['iter']=='2': r[int(row['query'])]=row
    return r

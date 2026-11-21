import sys,bisect
syms=[]
for l in open(sys.argv[1]):
    a,t,n=l.split()[:3]
    syms.append((int(a,16),n))
syms.sort(); keys=[a for a,n in syms]
for x in sys.argv[2:]:
    v=int(x,16); i=bisect.bisect_right(keys,v)-1
    print(x, syms[i][1]+'+'+hex(v-syms[i][0]))

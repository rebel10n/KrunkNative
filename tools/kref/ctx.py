import sys,re
pat=sys.argv[1]; n=int(sys.argv[2]); files=sys.argv[3:]
for f in files:
    s=open(f,encoding='utf-8',errors='replace').read()
    for i,m in enumerate(re.finditer(pat,s)):
        if i>=8: break
        a=max(0,m.start()-n); b=min(len(s),m.end()+n)
        print(f"== {f}:{m.start()}: ...{s[a:b]}...\n")

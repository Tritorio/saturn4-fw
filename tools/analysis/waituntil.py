import json,time,urllib.request,sys
target=int(sys.argv[1]); t_end=time.time()+float(sys.argv[2])
while time.time()<t_end:
    try:
        d=json.load(urllib.request.urlopen('http://192.168.0.44:3030/tmp/sgweb/live.json',timeout=4))
        w=d['wait']
        if w['last'] and w['last'][-1][0]>=target:
            print({k:w[k] for k in ('dw','wh','active','layer','bottom','el','max','n','ns','nm','nh','saved','prev')}); print('last',w['last'][-6:]); print('heat',d['heat']); break
    except Exception: pass
    time.sleep(1)

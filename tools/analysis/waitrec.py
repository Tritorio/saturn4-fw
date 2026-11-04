import json,time,urllib.request,sys
out=open(sys.argv[1],'a'); t_end=time.time()+3*3600; last_now=None
while time.time()<t_end:
    try:
        d=json.load(urllib.request.urlopen('http://192.168.0.44:3030/tmp/sgweb/live.json',timeout=4))
        if d['now']!=last_now:
            last_now=d['now']
            out.write(json.dumps({'now':d['now'],'job':d['job'],'wait':d['wait'],'heat':d['heat'],'r':d['r'][-3:]})+'\n'); out.flush()
            if len(d["wait"]["last"])>=int(sys.argv[2]):
                print('5 waits done', d['wait']); break
    except Exception as e:
        pass
    time.sleep(0.5)

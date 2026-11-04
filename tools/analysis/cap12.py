import sys,struct,json,re
sys.path.insert(0,'tools'); from sgcap_decode import records
data=open('backups/sglog/sgcap/cap_e41fd4ae_21541.bin','rb').read()
buf=b''; out=[]; t0=None
for ts,tid,typ,fd,p in records(data):
    if typ==5: t0=ts; print('START',p[:80]); continue
    if typ==0 and fd==33:
        buf+=p
        while True:
            i=buf.find(b'\x01\x03\x04')
            if i<0 or len(buf)<i+9: break
            v=struct.unpack_from('<i',buf,i+3)[0]; out.append((ts,v)); buf=buf[i+9:]
print(len(out),out[:3],out[-1])
json.dump(out,open(sys.argv[1],'w'))

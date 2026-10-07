exec(open('drive2.py').read())
import numpy as np
def kv(l): return dict(x.split('=',1) for x in l.split() if '=' in x)
m=size(); send('p',2.5)
for l in text(m).splitlines():
    if 'DG3_OBS' in l:
        d=kv(l[l.index('DG3_OBS'):]); print('state', {k:d.get(k) for k in ('gain','p50','coherence','lane')})
segs=[]
for k in range(40):
    m=size(); send('Q',0.8)
    for l in text(m).splitlines():
        if 'Q4RAW r=' in l:
            h=l.split()[-1]
            if len(h)!=128 or any(c not in '0123456789abcdef' for c in h): continue
            b=np.array([int(h[j:j+2],16) for j in range(0,128,2)])
            i=(b>>4); q=b&15; i=np.where(i>7,i-16,i)+0.5; q=np.where(q>7,q-16,q)+0.5
            z=i+1j*q; segs.append(z-z.mean())
segs=np.array(segs); print('segments', len(segs))
w=np.hanning(64)
P=np.mean(np.abs(np.fft.fft(segs*w,axis=1))**2,axis=0)
f=np.fft.fftfreq(64,1/40e6)
Pp=np.fft.fftshift(P); fp=np.fft.fftshift(f)/1e6
ref=np.mean(Pp[np.abs(fp)<4])
for lo,hi in ((0,4),(4,8),(8,10),(10,12),(12,14),(14,16),(16,18),(18,20)):
    sel=(np.abs(fp)>=lo)&(np.abs(fp)<hi)
    print(f'|f| {lo:2d}-{hi:2d} MHz: {10*np.log10(np.mean(Pp[sel])/ref):6.1f} dB')

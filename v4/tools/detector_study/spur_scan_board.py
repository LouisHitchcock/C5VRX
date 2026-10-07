exec(open('drive2.py').read())
import numpy as np, re, json
def kv(l): return dict(x.split('=',1) for x in l.split() if '=' in x)
def psd(n=10):
    segs=[]
    for k in range(n):
        m=size(); send('Q',0.7)
        for l in text(m).splitlines():
            if 'Q4RAW r=' in l:
                h=l.split()[-1]
                if len(h)!=128 or any(c not in '0123456789abcdef' for c in h): continue
                b=np.array([int(h[j:j+2],16) for j in range(0,128,2)])
                i=(b>>4); q=b&15; i=np.where(i>7,i-16,i)+0.5; q=np.where(q>7,q-16,q)+0.5
                z=i+1j*q; segs.append(z-z.mean())
    if len(segs)<8: return None
    w=np.hanning(64); P=np.mean(np.abs(np.fft.fft(np.array(segs)*w,axis=1))**2,axis=0)
    return np.fft.fftshift(P), np.fft.fftshift(np.fft.fftfreq(64,1/40e6))/1e6
res=[]
for band in range(6):
    for ch in range(8):
        m=size(); send('c' if (band or ch) else 'p',2.5)
        t=text(m); mm=re.findall(r'\((\d{4}) MHz\)', t)
        mhz=mm[-1] if mm else '?'
        r=psd()
        if r is None: print('no data', mhz); continue
        P,f=r
        inb=(np.abs(f)<=9.5)&(np.abs(f)>=0.7)
        med=np.median(P[inb]); k=np.argmax(np.where(inb,P,0))
        peak=10*np.log10(P[k]/med)
        res.append((mhz,round(float(f[k]),2),round(float(peak),1)))
        print(f'{mhz} MHz  biggest in-band peak {f[k]:+6.2f} MHz  {peak:5.1f} dB over median', flush=True)
    if band<5:
        m=size(); send('C',2.5)
json.dump(res,open('spurscan.json','w'))

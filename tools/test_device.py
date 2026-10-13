"""End-to-end test using Windows' built-in waveOut API, addressed to this device.
Does not change the system default output or install a runtime/driver.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import math
from pathlib import Path
import struct
import time
from device import connect,status,screenshot

class Caps(C.Structure):
    _fields_=[('mid',W.WORD),('pid',W.WORD),('version',W.DWORD),
              ('name',W.WCHAR*32),('formats',W.DWORD),('channels',W.WORD),
              ('reserved',W.WORD),('support',W.DWORD)]
class Format(C.Structure):
    _pack_=2
    _fields_=[('tag',W.WORD),('channels',W.WORD),('rate',W.DWORD),
              ('bytes',W.DWORD),('align',W.WORD),('bits',W.WORD),('extra',W.WORD)]
class Header(C.Structure):
    _fields_=[('data',C.c_void_p),('length',W.DWORD),('recorded',W.DWORD),
              ('user',C.c_size_t),('flags',W.DWORD),('loops',W.DWORD),
              ('next',C.c_void_p),('reserved',C.c_size_t)]
winmm=C.WinDLL('winmm')
winmm.waveOutGetNumDevs.restype=W.UINT
winmm.waveOutGetDevCapsW.argtypes=[C.c_size_t,C.POINTER(Caps),W.UINT]
winmm.waveOutOpen.argtypes=[C.POINTER(C.c_void_p),W.UINT,C.POINTER(Format),C.c_size_t,C.c_size_t,W.DWORD]
for fn in ['waveOutPrepareHeader','waveOutWrite','waveOutUnprepareHeader']:
    getattr(winmm,fn).argtypes=[C.c_void_p,C.POINTER(Header),W.UINT]
winmm.waveOutReset.argtypes=[C.c_void_p]
winmm.waveOutClose.argtypes=[C.c_void_p]

def checked(code):
    if code: raise RuntimeError(f'Windows waveOut error {code}')

def devices():
    result=[]
    for index in range(winmm.waveOutGetNumDevs()):
        caps=Caps(); checked(winmm.waveOutGetDevCapsW(index,C.byref(caps),C.sizeof(caps)))
        result.append((index,caps.name))
    return result

class Playback:
    def __init__(self,index,hz,seconds=2,opposite=False,multitone=False):
        self.handle=C.c_void_p()
        fmt=Format(1,2,48000,48000*4,4,16,0)
        checked(winmm.waveOutOpen(C.byref(self.handle),index,C.byref(fmt),0,0,0))
        pcm=bytearray(int(48000*seconds)*4)
        for i in range(len(pcm)//4):
            if multitone:
                a=sum(math.sin(2*math.pi*f*i/48000)*w for f,w in [(65,.2),(180,.15),(440,.2),(1000,.15),(3200,.15),(9600,.1)])
                b=sum(math.sin(2*math.pi*f*i/48000)*w for f,w in [(90,.2),(260,.15),(660,.2),(1800,.15),(5200,.1)])
                l=int(8192*a); r=int(8192*b)
            else:
                l=int(4096*math.sin(2*math.pi*hz*i/48000)); r=-l if opposite else l
            struct.pack_into('<hh',pcm,i*4,l,r)
        self.data=C.create_string_buffer(bytes(pcm))
        self.header=Header(C.cast(self.data,C.c_void_p),len(pcm),0,0,0,0,None,0)
        checked(winmm.waveOutPrepareHeader(self.handle,C.byref(self.header),C.sizeof(Header)))
        checked(winmm.waveOutWrite(self.handle,C.byref(self.header),C.sizeof(Header)))
    def close(self):
        checked(winmm.waveOutReset(self.handle))
        checked(winmm.waveOutUnprepareHeader(self.handle,C.byref(self.header),C.sizeof(Header)))
        checked(winmm.waveOutClose(self.handle))

if __name__=='__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('--list',action='store_true'); parser.add_argument('--port'); parser.add_argument('--seconds',type=int,default=3)
    args=parser.parse_args(); available=devices(); print(available,flush=True)
    if args.list: raise SystemExit()
    found=[i for i,n in available if 'CoreS3 USB Spectrum' in n]
    if len(found)!=1: raise RuntimeError(f'Expected one audio endpoint, found {found}')
    out=Path(__file__).resolve().parents[1]/'verification'; out.mkdir(exist_ok=True)
    results=[]
    with connect(args.port) as s:
        for hz,opposite in [(440,False),(1000,False),(4000,False),(1000,True),(0,False)]:
            play=Playback(found[0],hz,args.seconds,opposite)
            try:
                # Allow any Windows endpoint-open notification sound to finish.
                time.sleep(1.5 if hz==0 else .6)
                readings=[]
                for _ in range(3): readings.append(status(s)); time.sleep(.15)
                result={'tone_hz':hz,'opposite_phase':opposite,'readings':readings}
                result['passed']=all(abs(v['peak_hz']-hz)<5 for v in readings) if hz else all(v['left_dbfs']<=-110 and v['peak_hz']==0 for v in readings)
                results.append(result); print(json.dumps(result),flush=True)
            finally: play.close()
        play=Playback(found[0],0,8,multitone=True)
        try:
            time.sleep(.5); screenshot(s,out/'spectrum-live.png'); results.append({'multitone':status(s)})
        finally: play.close()
        time.sleep(.4); screenshot(s,out/'spectrum-idle.png'); results.append({'idle':status(s)})
    (out/'usb-audio-test.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
    if not all(r.get('passed',True) for r in results): raise SystemExit('Tone test FAILED')
    print('USB audio tone tests passed.',flush=True)

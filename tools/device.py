"""Development-only diagnostics. Normal operation needs no PC application."""
import argparse
import json
import pathlib
import time
import serial
from serial.tools import list_ports

def connect(port=None):
    if not port:
        candidates=[p.device for p in list_ports.comports() if p.vid==0x303a and p.pid==0x8000]
        if len(candidates)!=1:
            raise RuntimeError(f'Expected one Spectrum device, found {candidates}')
        port=candidates[0]
    s=serial.Serial(port,115200,timeout=4)
    s.dtr=True
    time.sleep(.15)
    s.reset_input_buffer()
    return s

def status(s):
    s.write(b's')
    return json.loads(s.readline())

def screenshot(s,path):
    from PIL import Image
    s.write(b'p')
    header=[s.readline() for _ in range(3)]
    if header!=[b'P6\n',b'320 240\n',b'255\n']:
        raise RuntimeError(f'Invalid image header: {header}')
    pixels=s.read(320*240*3)
    if len(pixels)!=320*240*3:
        raise RuntimeError(f'Incomplete framebuffer: {len(pixels)} bytes')
    Image.frombytes('RGB',(320,240),pixels).save(path)
    return str(path)

if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('action',choices=['status','screenshot','mute','hold','wave','verify-display'])
    p.add_argument('--port')
    p.add_argument('--output',default='device-screen.png')
    args=p.parse_args()
    with connect(args.port) as s:
        if args.action=='status': print(json.dumps(status(s),indent=2))
        elif args.action=='screenshot': print(screenshot(s,pathlib.Path(args.output)))
        else:
            s.write({'mute':b'm','hold':b'h','wave':b'w','verify-display':b'v'}[args.action])
            print(s.readline().decode())

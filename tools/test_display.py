"""Hardware checks for partial DMA updates, waveform, HOLD and idle reset."""
import json
from pathlib import Path
import time
from device import connect, status, screenshot
from test_device import Playback, devices


def command(port, value):
    port.write(value)
    return json.loads(port.readline())


def main():
    endpoints=[i for i,n in devices() if 'CoreS3 USB Spectrum' in n]
    if len(endpoints)!=1:
        raise RuntimeError('Expected one CoreS3 USB Spectrum endpoint')
    out=Path(__file__).resolve().parents[1]/'verification'
    results=[]
    with connect() as port:
        original=status(port)
        if original['hold']:
            raise RuntimeError('Disable HOLD before running the display tests')
        if not original['local_mute']: command(port,b'm')
        if original['waveform']: command(port,b'w')
        play=None
        try:
            play=Playback(endpoints[0],0,12,motion=True)
            time.sleep(1.5)
            for mode in ['bars','waveform']:
                if mode=='waveform': command(port,b'w')
                time.sleep(.3)
                for _ in range(3):
                    result=command(port,b'v')
                    result['mode']=mode
                    results.append(result)
                    assert result['lcd_mismatched_pixels']==0, result
                    time.sleep(.15)
                screenshot(port,out/f'optimized-{mode}.png')
            held=command(port,b'h')
            time.sleep(.25)
            assert status(port)['peak_hz']==held['peak_hz'], 'HOLD changed the analysis'
            play.close(); play=None
            time.sleep(.4)
            assert not status(port)['audio'], 'Audio remained active after playback stopped'
            command(port,b'h'); time.sleep(.1)
            idle=status(port)
            assert idle['peak_hz']==0 and idle['left_dbfs']<=-119, 'Stale held analysis after idle'
            assert idle['dropped_bytes']==original['dropped_bytes'], 'Speaker queue dropped audio'
            results.append({'hold_and_idle_reset_passed':True,'dropped_bytes':0})
        finally:
            if play: play.close()
            current=status(port)
            if current['hold']!=original['hold']: command(port,b'h')
            if current['waveform']!=original['waveform']: command(port,b'w')
            if current['local_mute']!=original['local_mute']: command(port,b'm')
    out.mkdir(exist_ok=True)
    (out/'display-test.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(results,indent=2))


if __name__=='__main__':
    main()

"""Measure completed display updates while sending moving stereo test tones.

Uses Windows waveOut and the development CDC port. Temporarily mutes the
onboard speaker and restores its state; does not change the Windows output.
"""
import argparse
import json
from pathlib import Path
import time
from device import connect, status
from test_device import Playback, devices


def toggle(port, command):
    port.write(command)
    return json.loads(port.readline())


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=8)
    parser.add_argument('--port')
    parser.add_argument('--waveform', action='store_true')
    parser.add_argument('--output', type=Path)
    args=parser.parse_args()
    if not 2 <= args.seconds <= 20:
        parser.error('--seconds must be between 2 and 20')
    endpoints=[i for i,n in devices() if 'CoreS3 USB Spectrum' in n]
    if len(endpoints)!=1:
        raise RuntimeError('Expected one CoreS3 USB Spectrum audio endpoint')
    with connect(args.port) as port:
        original=status(port)
        if original['hold']:
            raise RuntimeError('Disable HOLD before measuring live updates')
        if not original['local_mute']:
            toggle(port,b'm')
        switch_waveform=args.waveform and not original.get('waveform',False)
        if switch_waveform:
            toggle(port,b'w')
        play=None
        try:
            play=Playback(endpoints[0],0,args.seconds+3,motion=True)
            time.sleep(.6)
            start=status(port); started=time.perf_counter()
            time.sleep(args.seconds)
            end=status(port); elapsed=time.perf_counter()-started
            result={'firmware':end['firmware'], 'workload':'moving stereo chirps',
                    'display_mode':'waveform' if end.get('waveform',False) else 'bars',
                    'elapsed_s':round(elapsed,4),
                    'display_updates_per_second':round((end['draws']-start['draws'])/elapsed,2),
                    'received_frames':end['frames']-start['frames'],
                    'dropped_bytes':end['dropped_bytes']-start['dropped_bytes'],
                    'start':start,'end':end}
            assert start['audio'] and end['audio'], 'Audio stopped during benchmark'
            assert result['received_frames']>48000*(elapsed-.5), 'Audio stream incomplete'
            assert result['dropped_bytes']==0, 'Speaker queue dropped audio'
            if args.output:
                args.output.parent.mkdir(exist_ok=True,parents=True)
                args.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
            print(json.dumps(result,indent=2))
        finally:
            if play:
                play.close()
            if switch_waveform:
                toggle(port,b'w')
            if not original['local_mute']:
                toggle(port,b'm')


if __name__=='__main__':
    main()

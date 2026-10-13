"""Package an already merged firmware image with instructions and notices.

Usage: python tools/package_release.py dist/cores3-usb-spectrum-v1.0.0.bin
Outputs a sibling ZIP and SHA256SUMS.txt. Does not flash a device.
"""
import argparse
import hashlib
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED


def checksum(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('firmware', type=Path)
    args = parser.parse_args()
    firmware = args.firmware.resolve()
    data = firmware.read_bytes()
    if (len(data) <= 0x10000 or data[0] != 0xE9
            or data[0x8000:0x8002] != b'\xaa\x50'
            or data[0x10000] != 0xE9):
        parser.error('Expected a merged ESP32-S3 image with partitions at 0x8000 and app at 0x10000')
    root = Path(__file__).resolve().parent.parent
    files = ['README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.txt',
             'verification/summary.json', 'verification/spectrum-live.png',
             'verification/spectrum-idle.png']
    for name in files:
        if not (root/name).is_file():
            parser.error(f'Missing package file: {name}')
    archive = firmware.with_suffix('.zip')
    bin_checksum = f'{checksum(firmware)}  {firmware.name}\n'
    with ZipFile(archive, 'w', ZIP_DEFLATED) as bundle:
        bundle.write(firmware, firmware.name)
        for name in files:
            bundle.write(root/name, name)
        bundle.writestr('SHA256SUMS.txt', bin_checksum)
    with ZipFile(archive) as bundle:
        if bundle.testzip() is not None:
            raise RuntimeError('ZIP verification failed')
        assert bundle.read(firmware.name) == data
    sums = firmware.parent/'SHA256SUMS.txt'
    sums.write_text(bin_checksum + f'{checksum(archive)}  {archive.name}\n',
                    encoding='ascii', newline='\n')
    print(f'Created {archive.name} ({archive.stat().st_size:,} bytes)')
    print(sums.read_text(), end='')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Exercise Metal -> AVAssetWriter, including abrupt exit, with independent decoding."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='Fresh output directory')
    parser.add_argument('--ffmpeg', type=Path, required=True, help='Independent FFmpeg decoder')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).with_name('AppleFrameVideoFixture.mm')
    executable = output / 'fixture'
    frameworks = ('Foundation', 'Metal', 'AVFoundation', 'QuartzCore', 'CoreMedia', 'CoreVideo')
    command = ['xcrun', 'clang++', '-std=c++20', '-fno-objc-arc', str(source), '-o', str(executable)]
    for framework in frameworks:
        command.extend(('-framework', framework))
    with (output / 'build.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
    for name, extra, expected in (('colors', [], 0), ('empty', ['empty'], 0),
                                   ('cancelled', ['cancel'], 0),
                                   ('interrupted', ['interrupt'], 17)):
        with (output / (name + '.log')).open('w') as log:
            run = subprocess.run([str(executable), str(output / name), *extra],
                                 stdout=log, stderr=subprocess.STDOUT, timeout=20)
        assert run.returncode == expected, (name, run.returncode)
    results = {}
    stride = 128 * 96 * 3
    for name in ('colors', 'interrupted'):
        decoded = subprocess.run([str(args.ffmpeg.resolve()), '-v', 'error', '-i',
            str(output / name / 'viewport.mp4'), '-vsync', '0', '-f', 'rawvideo',
            '-pix_fmt', 'rgb24', '-'], capture_output=True, check=True, timeout=20)
        assert len(decoded.stdout) % stride == 0
        count = len(decoded.stdout) // stride
        assert count == 18 if name == 'colors' else 12 <= count <= 160, (name, count)
        checked = []
        for index in (2, 8, min(14, count - 1)):
            wanted = (255, 0, 0) if index < 6 else ((0, 0, 255) if index < 12 else (0, 255, 255))
            frame = decoded.stdout[index * stride:(index + 1) * stride]
            rgb = [sum(frame[channel::3]) / (128 * 96) for channel in range(3)]
            assert max(abs(a-b) for a, b in zip(rgb, wanted)) <= 8, (name, index, rgb)
            checked.append({'index': index, 'rgbMean': rgb})
        results[name] = {'decodedFrames': count, 'checkedColors': checked}
    assert not (output / 'interrupted' / 'recording.json').exists(), 'Abrupt exit accidentally finalized'
    result = {'passed': True, 'independentDecoder': str(args.ffmpeg.resolve()),
              'maximumAllowedColorError': 8, 'results': results,
              'interruptedWithoutFinish': True, 'cancelStopsCapture': True,
              'gameplayTested': False}
    (output / 'validation.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

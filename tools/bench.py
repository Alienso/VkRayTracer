# Runs benches (benches/*.json, see source/app/Bench.h) against one or two builds and compares them.
#
#   python tools/bench.py run benches/sponza.json benches/bistro_night.json --a cmake-build-release
#           --b ../other-checkout/cmake-build-release --runs 3
#   python tools/bench.py run benches/sponza.json --a cmake-build-release --b cmake-build-release
#           --b-args "--shader-dir C:/temp/new_shaders"           (a shader change, same exe)
#   python tools/bench.py show bench_runs/2026-10-15_21-00-00      (the table of an earlier run again)
#   python tools/bench.py images a.png b.png                       (or two folders of captures)
#
# A and B take turns (A B A B ... for each bench), so a GPU that warms up or a background task affects both. The table
# shows each number's median over the runs and its spread (largest - smallest): a difference smaller than the spreads
# is noise (GPU times move by about 0.4 ms between runs).
#
# Image checks: captures of A and B (first runs) are compared; with --reference DIR also each run's against
# DIR/<bench>/capture_<frame>.png, and --save-reference DIR stores A's first run there. A difference counts when the
# mean error (0-255) or the share of pixels off by more than --changed-by is beyond the tolerance (--tolerance-mean,
# --tolerance-changed); the exit code is then 1.
#
# Needs numpy (reads the PNGs itself).
import argparse, datetime, json, os, shlex, shutil, subprocess, sys, time, zlib
import numpy as np

PASSES = ['cache', 'restir', 'trace', 'dlss', 'exposure', 'display', 'total']


# PNG reading (8-bit gray / RGB / RGBA, not interlaced: what stb_image_write and most tools write)

def read_png(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: not a PNG')
    pos, idat = 8, []
    width = height = channels = None
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], 'big')
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b'IHDR':
            width, height = int.from_bytes(body[0:4], 'big'), int.from_bytes(body[4:8], 'big')
            depth, color, interlace = body[8], body[9], body[12]
            if depth != 8 or interlace != 0 or color not in (0, 2, 6):
                raise ValueError(f'{path}: only 8-bit, not interlaced gray / RGB / RGBA PNGs')
            channels = {0: 1, 2: 3, 6: 4}[color]
        elif tag == b'IDAT':
            idat.append(body)
        elif tag == b'IEND':
            break
    raw = np.frombuffer(zlib.decompress(b''.join(idat)), np.uint8).reshape(height, 1 + width * channels)
    filters, rows = raw[:, 0], raw[:, 1:].astype(np.int32)
    out = np.zeros((height, width * channels), np.int32)
    previous = np.zeros(width * channels, np.int32)
    c = channels
    for y in range(height):
        row, kind = rows[y], filters[y]
        if kind == 0:
            line = row
        elif kind == 1:  # Sub: a running sum per channel
            line = np.cumsum(row.reshape(width, c), axis=0).reshape(-1) & 255
        elif kind == 2:  # Up
            line = (row + previous) & 255
        else:  # Average, Paeth: each pixel needs the one to its left
            line = [0] * (width * c)
            up = previous.tolist()
            r = row.tolist()
            for i in range(width * c):
                left = line[i - c] if i >= c else 0
                if kind == 3:
                    line[i] = (r[i] + ((left + up[i]) >> 1)) & 255
                else:
                    upLeft = up[i - c] if i >= c else 0
                    p = left + up[i] - upLeft
                    pa, pb, pc = abs(p - left), abs(p - up[i]), abs(p - upLeft)
                    predictor = left if pa <= pb and pa <= pc else (up[i] if pb <= pc else upLeft)
                    line[i] = (r[i] + predictor) & 255
            line = np.array(line, np.int32)
        out[y] = line
        previous = line
    return out.reshape(height, width, channels).astype(np.uint8)


def compare_images(a_path, b_path, changed_by):
    """Mean absolute error (0-255, RGB), largest error, share of pixels (%) changed by more than changed_by in some
    channel, PSNR (dB)."""
    a, b = read_png(a_path)[..., :3].astype(np.int32), read_png(b_path)[..., :3].astype(np.int32)
    if a.shape != b.shape:
        return {'error': f'sizes differ: {a.shape[1]}x{a.shape[0]} and {b.shape[1]}x{b.shape[0]}'}
    d = np.abs(a - b)
    mse = float(np.mean(d.astype(np.float64) ** 2))
    return {'mean': float(d.mean()), 'max': int(d.max()), 'changed': float(np.mean(d.max(axis=2) > changed_by) * 100.0),
            'psnr': float('inf') if mse == 0 else 10.0 * np.log10(255.0 ** 2 / mse)}


def image_line(name, result, args):
    if 'error' in result:
        return f'  {name}: {result["error"]}', True
    bad = result['mean'] > args.tolerance_mean or result['changed'] > args.tolerance_changed
    psnr = 'identical' if result['max'] == 0 else f'PSNR {result["psnr"]:.1f} dB'
    return (f'  {name}: mean error {result["mean"]:.3f}, max {result["max"]}, {result["changed"]:.3f}% of pixels '
            f'off by more than {args.changed_by}, {psnr}{"  <-- beyond tolerance" if bad else ""}'), bad


# Running

def runtime_environment(build):
    """The environment to run the build's exe in: the compiler's bin folder (from CMakeCache.txt) first in PATH, so
    the exe loads the libstdc++ it was built with. Another one found first (Git's mingw64\\bin has one) crashes it at
    startup."""
    env = dict(os.environ)
    cache = os.path.join(build, 'CMakeCache.txt')
    if os.path.exists(cache):
        with open(cache, encoding='utf-8', errors='replace') as f:
            for line in f:
                if line.startswith('CMAKE_CXX_COMPILER:'):
                    compiler_bin = os.path.dirname(line.split('=', 1)[1].strip())
                    env['PATH'] = os.path.normpath(compiler_bin) + os.pathsep + env.get('PATH', '')
                    break
    return env


def run_bench(build, extra_args, bench, out_dir, timeout):
    """Runs the exe in its build folder (it reads its shaders from there) and returns results.json, or None."""
    exe = os.path.join(build, 'vulkanDemo.exe')
    os.makedirs(out_dir, exist_ok=True)
    command = [exe, '--bench', os.path.abspath(bench), '--bench-out', os.path.abspath(out_dir)] + extra_args
    with open(os.path.join(out_dir, 'stdout.txt'), 'w') as out, open(os.path.join(out_dir, 'stderr.txt'), 'w') as err:
        try:
            code = subprocess.run(command, cwd=build, stdout=out, stderr=err, timeout=timeout,
                                  env=runtime_environment(build)).returncode
        except subprocess.TimeoutExpired:
            print(f'    timed out after {timeout} s')
            return None
    path = os.path.join(out_dir, 'results.json')
    if not os.path.exists(path):
        print(f'    exit code {code}, no results (see {out_dir}/stderr.txt)')
        return None
    with open(path) as f:
        results = json.load(f)
    if 'error' in results:
        print(f'    failed: {results["error"]}')
        return None
    return results


def numbers(results):
    """The numbers the table compares, by row name."""
    row = {}
    for p in PASSES:
        mean = results.get('gpuMs', {}).get(p, {}).get('mean')
        if mean is not None and (mean > 0.005 or p == 'total'):
            row[f'gpu {p} ms'] = mean
    for phase in ['work', 'update', 'game', 'ui', 'render', 'wait']:
        mean = results.get('cpuMs', {}).get(phase, {}).get('mean')
        if mean is not None:
            row[f'cpu {phase} ms'] = mean
    row['frame ms (wall)'] = results.get('frameMs')
    memory = results.get('memory', {})
    if 'privateBytes' in memory:
        row['private MB'] = memory['privateBytes'] / 2 ** 20
    if 'gpuUsedBytes' in memory:
        row['GPU memory MB'] = memory['gpuUsedBytes'] / 2 ** 20
    row['loaded s'] = results.get('startup', {}).get('loadedSeconds')  # files read
    row['ready s'] = results.get('startup', {}).get('readySeconds')    # the first frame with all of it drawn
    noise = results.get('noise', {}).get('frames', [])
    if noise:
        row['noise relMSE f1'] = noise[0]['relativeMse']
        row['noise relMSE last'] = noise[-1]['relativeMse']
        row['noise brightness'] = noise[-1]['brightness']
    stats = results.get('pipelineStatistics', {}).get('Image paths', {})
    for key in ('Register Count', 'Shared Memory Size', 'Binary Size'):
        if key in stats and isinstance(stats[key], (int, float)):
            row[f'trace {key.lower()}'] = stats[key]
    return {k: v for k, v in row.items() if v is not None}


def print_table(collected, labels):
    """collected[bench][label] = [results of each run]"""
    for bench, by_label in collected.items():
        print(f'\n{bench}')
        rows = {}
        for label in labels:
            for results in by_label.get(label, []):
                for name, value in numbers(results).items():
                    rows.setdefault(name, {}).setdefault(label, []).append(value)
        header = f'  {"":22}' + ''.join(f'{label + " median":>14}{"spread":>10}' for label in labels)
        if len(labels) == 2:
            header += f'{"B vs A":>10}'
        print(header)
        for name, by in rows.items():
            line = f'  {name:22}'
            medians = []
            for label in labels:
                values = by.get(label, [])
                if not values:
                    line += f'{"-":>14}{"":>10}'
                    medians.append(None)
                    continue
                median = float(np.median(values))
                medians.append((median, max(values) - min(values)))
                small = abs(median) < 0.01 and median != 0
                line += (f'{median:>14.3e}' if small else f'{median:>14.3f}') + (
                    f'{max(values) - min(values):>10.3g}' if len(values) > 1 else f'{"":>10}')
            if len(labels) == 2 and all(medians) and medians[0][0] != 0:
                (a, spread_a), (b, spread_b) = medians
                change = (b - a) / abs(a) * 100.0
                beyond = abs(b - a) > max(spread_a, spread_b)
                line += f'{change:>+9.1f}%' + (' *' if beyond and change != 0 else '')
            print(line)
    if len(labels) == 2:
        print('\n  * : the difference is larger than the spread of either side\'s runs')


def load_runs(out_root):
    """collected[bench][label] = [results...] from an output folder of `run`."""
    collected, labels = {}, []
    for label in sorted(os.listdir(out_root)):
        label_dir = os.path.join(out_root, label)
        if not os.path.isdir(label_dir):
            continue
        labels.append(label)
        for bench in sorted(os.listdir(label_dir)):
            for run in sorted(os.listdir(os.path.join(label_dir, bench))):
                path = os.path.join(label_dir, bench, run, 'results.json')
                if os.path.exists(path):
                    with open(path) as f:
                        results = json.load(f)
                    if 'error' not in results:
                        collected.setdefault(bench, {}).setdefault(label, []).append(results)
    return collected, labels


def captures_of(directory):
    return sorted(f for f in os.listdir(directory) if f.startswith('capture_') and f.endswith('.png'))


def command_run(args):
    sides = [('A', args.a, shlex.split(args.a_args))]
    if args.b:
        sides.append(('B', args.b, shlex.split(args.b_args)))
    for _, build, _ in sides:
        if not os.path.exists(os.path.join(build, 'vulkanDemo.exe')):
            sys.exit(f'no vulkanDemo.exe in {build}')
    out_root = args.out or os.path.join('bench_runs', datetime.datetime.now().strftime('%Y-%m-%d_%H-%M-%S'))
    collected = {}
    failed = False
    for bench in args.benches:
        name = os.path.splitext(os.path.basename(bench))[0]
        for run in range(1, args.runs + 1):
            for label, build, extra in sides:
                out_dir = os.path.join(out_root, label, name, f'run{run}')
                print(f'{name} {label} run {run}/{args.runs}')
                started = time.time()
                results = run_bench(build, extra, bench, out_dir, args.timeout)
                if results is None:
                    failed = True
                    continue
                print(f'    {time.time() - started:.0f} s, GPU total {results["gpuMs"].get("total", {}).get("mean", 0):.2f} ms')
                collected.setdefault(name, {}).setdefault(label, []).append(results)
    labels = [label for label, _, _ in sides]
    print_table(collected, labels)

    # Images
    image_problem = False
    lines = []
    for bench in args.benches:
        name = os.path.splitext(os.path.basename(bench))[0]
        first = {label: os.path.join(out_root, label, name, 'run1') for label in labels}
        if not os.path.isdir(first['A']):
            continue
        for capture in captures_of(first['A']):
            if len(labels) == 2 and os.path.exists(os.path.join(first['B'], capture)):
                line, bad = image_line(f'{name} {capture} B vs A', compare_images(
                    os.path.join(first['A'], capture), os.path.join(first['B'], capture), args.changed_by), args)
                lines.append(line)
                image_problem |= bad
            if args.reference:
                reference = os.path.join(args.reference, name, capture)
                if not os.path.exists(reference):
                    lines.append(f'  {name} {capture}: no reference ({reference})')
                    continue
                for label in labels:
                    for run_dir in sorted(os.listdir(os.path.join(out_root, label, name))):
                        path = os.path.join(out_root, label, name, run_dir, capture)
                        if os.path.exists(path):
                            line, bad = image_line(f'{name} {capture} {label} {run_dir} vs reference',
                                                   compare_images(reference, path, args.changed_by), args)
                            lines.append(line)
                            image_problem |= bad
            if args.save_reference:
                os.makedirs(os.path.join(args.save_reference, name), exist_ok=True)
                shutil.copy2(os.path.join(first['A'], capture), os.path.join(args.save_reference, name, capture))
                lines.append(f'  {name} {capture}: saved as reference in {args.save_reference}')
    if lines:
        print('\nImages')
        print('\n'.join(lines))
    print(f'\nresults in {out_root}')
    sys.exit(1 if failed or image_problem else 0)


def command_show(args):
    for out_root in args.folders:
        collected, labels = load_runs(out_root)
        print(out_root)
        print_table(collected, labels)


def command_images(args):
    if os.path.isdir(args.a):
        pairs = [(os.path.join(args.a, f), os.path.join(args.b, f), f) for f in captures_of(args.a)
                 if os.path.exists(os.path.join(args.b, f))]
    else:
        pairs = [(args.a, args.b, os.path.basename(args.b))]
    problem = False
    for a, b, name in pairs:
        line, bad = image_line(name, compare_images(a, b, args.changed_by), args)
        print(line)
        problem |= bad
    sys.exit(1 if problem else 0)


def main():
    parser = argparse.ArgumentParser(description='Runs and compares vulkanDemo benches (see source/app/Bench.h)')
    sub = parser.add_subparsers(dest='command', required=True)

    def tolerances(p):
        # Run to run (same build): Accumulation frames differ in ~0.001% of pixels; DLSS frames (DLAA, Sponza) by a
        # mean of 0.17, 0.3% of pixels by more than 8
        p.add_argument('--tolerance-mean', type=float, default=0.5, help='largest mean error (0-255) still the same')
        p.add_argument('--changed-by', type=int, default=8, help='a pixel counts as changed beyond this (0-255)')
        p.add_argument('--tolerance-changed', type=float, default=1.0,
                       help='largest share of pixels (%%) changed still the same')

    run = sub.add_parser('run', help='run benches against one or two builds')
    run.add_argument('benches', nargs='+')
    run.add_argument('--a', required=True, help='build folder with vulkanDemo.exe (run there: its shaders)')
    run.add_argument('--a-args', default='', help='more options for A, e.g. "--shader-dir DIR --no-dlss"')
    run.add_argument('--b', help='a second build folder to compare with (may be the same as A, with other --b-args)')
    run.add_argument('--b-args', default='')
    run.add_argument('--runs', type=int, default=3)
    run.add_argument('--out', help='output folder (default bench_runs/<date_time>)')
    run.add_argument('--timeout', type=int, default=900, help='seconds per run')
    run.add_argument('--reference', help='compare captures with DIR/<bench>/capture_<frame>.png')
    run.add_argument('--save-reference', help="store A's first run's captures as references in DIR")
    tolerances(run)
    run.set_defaults(func=command_run)

    show = sub.add_parser('show', help='the table of earlier runs')
    show.add_argument('folders', nargs='+')
    show.set_defaults(func=command_show)

    images = sub.add_parser('images', help='compare two PNGs, or the captures in two folders')
    images.add_argument('a')
    images.add_argument('b')
    tolerances(images)
    images.set_defaults(func=command_images)

    args = parser.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()

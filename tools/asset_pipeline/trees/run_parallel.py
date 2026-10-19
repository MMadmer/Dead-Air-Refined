"""Run make_trees.py one family per process, N at a time."""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def main():
    script = os.environ.get('TREES_SCRIPT', 'make_trees.py')
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 6
    if script == 'seasons.py':
        import seasons
        fams = sys.argv[2:] or list(seasons.all_configs())
        if os.environ.get('TREES_LIST'):
            fams = open(os.environ['TREES_LIST']).read().split()
        logdir = os.path.join(seasons.OUT, 'logs')
    else:
        import make_trees
        fams = sys.argv[2:] or list(make_trees.FAMILIES)
        logdir = os.path.join(make_trees.OUT, 'logs')
    os.makedirs(logdir, exist_ok=True)
    running = []
    t0 = time.time()
    while fams or running:
        while fams and len(running) < n:
            f = fams.pop(0)
            log = open(os.path.join(logdir, f + '.log'), 'w')
            extra = os.environ.get('TREES_ARGS', '').split()
            running.append((f, subprocess.Popen([sys.executable, os.path.join(HERE, script)] + extra + [f], stdout=log, stderr=subprocess.STDOUT), log))
        for item in list(running):
            f, p, log = item
            if p.poll() is not None:
                log.close()
                running.remove(item)
                print(f'{f} exit={p.returncode} at {time.time() - t0:.0f}s', flush=True)
        time.sleep(1)


if __name__ == '__main__':
    main()

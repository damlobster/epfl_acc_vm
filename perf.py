#!/usr/bin/env python3

import sys
import os
from time import time
import numpy as np

import argparse

parser = argparse.ArgumentParser(description='Do performance tests of L3 vm')
parser.add_argument('-m', dest='heap', default=1000000, help="Heap size in bytes")
parser.add_argument('-n', dest='n', default=1, help='Number of iterations', type=int)
parser.add_argument('-b', dest='make', nargs='*', default='', help='Arguments passed to make')
parser.add_argument('-s', "--silent", dest='silent', action="store_true", default=False, help='Redirect output to /dev/null')
parser.add_argument(dest='l3', nargs='+', default='', help='The L3/ASM file and its argument')

args = parser.parse_args()
if 0 != os.system('make ' + ' '.join(args.make)):
    exit()

times = []
for i in range(args.n):
    start = time()
    os.system('echo {} | bin/vm test/{}.asm {}'.format(' '.join(args.l3[1:]), args.l3[0], ' > /dev/null' if args.silent else ''))
    times += [time()-start]
    print('.', end='', flush=True)

print('\nElapsed times: {:.6f} +/- {:.6f}'.format(np.mean(times), np.std(times)))

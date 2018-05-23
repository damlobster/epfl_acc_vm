#!/usr/bin/python

import sys
import os
from time import time

args = sys.argv[1:]

make_arg = ''
if args[0].startswith('-'):
    make_arg = args[0][1:]
    args = args[1:]

os.system('make ' + make_arg)

# Start Timestamp
start = time()

# Commands here (eg: TCP connect test or something useful)
os.system('echo {} | bin/vm test/{}.asm'.format(' '.join(args[1:]), args[0]))

print('\nElapsed time = {}'.format(time()-start))

#!/usr/bin/python

import sys
import os
from time import time

os.system('make stats')

# Start Timestamp
start = time()

# Commands here (eg: TCP connect test or something useful)
os.system('echo {} | bin/vm test/{}.asm'.format(' '.join(sys.argv[2:]), sys.argv[1]))

print('Elapsed time = {}'.format(time()-start))

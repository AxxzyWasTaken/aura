#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import sys, pathlib
sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

from aura.setup import *
from aura.hw.pmu import PMU

PMU(u).reset_panic_counter()

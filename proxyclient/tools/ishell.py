#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import pathlib
import sys

sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

from aura.setup import *
from aura.ishell import run_ishell

run_ishell(globals(), msg="Have fun!")

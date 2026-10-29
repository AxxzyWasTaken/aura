#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

import sys, pathlib
sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

from aura.setup import *
from aura.constructutils import Ver
from aura.utils import *

Ver.set_version(u)

from aura.shell import run_shell

from aura.fw.agx import Agx

p.pmgr_adt_power_enable("/arm-io/gfx-asc")
p.pmgr_adt_power_enable("/arm-io/sgx")

agx = Agx(u)
agx.verbose = 10

#agx.uat.dump(0)

agx.build_initdata()

run_shell(globals(), msg="Have fun!")

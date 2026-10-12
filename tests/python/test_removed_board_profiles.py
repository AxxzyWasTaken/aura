# SPDX-License-Identifier: MIT
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def test_default_release_has_no_board_profile_or_implicit_proxy_wait(tmp_path):
    config = '#define RELEASE\n#define CHAINLOADING\n#include "config.h"\n'
    result = subprocess.run(['cc','-E','-dM','-x','c','-I',str(ROOT),'-'],
                            input=config,text=True,check=True,capture_output=True)
    for option in ('J700_CDC_PROXY','J700_CDC_AUTOSTART','J700_CDC_STAGE1',
                   'T8140_KIS_PROXY','J700_ESP_STAGE2','EARLY_PROXY_TIMEOUT'):
        assert f'#define {option}' not in result.stdout
    assert '#define USE_FB' in result.stdout
    assert '#define FB_SILENT_MODE' in result.stdout

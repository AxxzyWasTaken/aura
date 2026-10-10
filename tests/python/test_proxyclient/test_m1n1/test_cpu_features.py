# SPDX-License-Identifier: GPL-2.0-only

from proxyclient.m1n1.proxy import CPUFeatures


def test_cpu_features_wfi_flag_fits_existing_abi():
    assert CPUFeatures.sizeof() == 20
    features = CPUFeatures.parse(bytes(19) + b"\x01")
    assert features.unsafe_wfi is True
    assert features.apple_sysregs_unlocked is False

# SPDX-License-Identifier: GPL-2.0-only
import struct

import pytest

from proxyclient.aura.proxy import AuraProxy, ProxyRemoteError


class FakeInterface:
    def __init__(self, status=0, retval=0):
        self.status = status
        self.retval = retval
        self.requests = []

    def proxyreq(self, request, **kwargs):
        opcode, *args = struct.unpack("<7Q", request)
        self.requests.append((opcode, args))
        return struct.pack("<QqQ", opcode, self.status, self.retval)


def test_cdc_schedule_and_status_use_pinned_wire_opcodes():
    iface = FakeInterface(retval=0x502)
    proxy = AuraProxy(iface)
    assert proxy.cdc_schedule(5000, 0x6) == 0x502
    assert proxy.cdc_status() == 0x502
    assert iface.requests == [
        (0x907, [5000, 0, 6, 0, 0, 0]),
        (0x908, [0, 0, 0, 0, 0, 0]),
    ]


def test_rejected_schedule_is_a_remote_error():
    proxy = AuraProxy(FakeInterface(status=-2))
    with pytest.raises(ProxyRemoteError, match="Remote operation failed"):
        proxy.cdc_schedule(500, 0x6)

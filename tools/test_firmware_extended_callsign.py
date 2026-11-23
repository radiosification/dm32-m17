#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the Home screen with the local firmware VM after test-host and firmware."""

import subprocess
from pathlib import Path
from tempfile import TemporaryDirectory


def expect_label(vm, label, blocks):
    draw_text = vm.syms['gfx_text']
    seen = set()
    vm.rsp.bp(draw_text)
    try:
        for _ in range(512):
            vm.rsp.resume()
            assert vm.rsp.hit() == draw_text
            address = vm.rsp.reg(3)  # gfx_text's fourth argument is the label.
            drawn = vm.rsp.mem(address, 24).split(b'\0', 1)[0]
            if drawn == label:
                count = vm.value('m17_frames')
                assert count in blocks, (label, count)
                return
            seen.add(drawn)
            vm.rsp.bp(draw_text, False)
            vm.rsp.command('s', wait=False)
            vm.rsp.hit()
            vm.rsp.bp(draw_text)
        raise AssertionError(f'Expected {label!r}, saw {sorted(seen)!r}')
    finally:
        vm.rsp.bp(draw_text, False)


def expect_filtered_call(vm):
    # The receiver should see the intervening CAN 1 call while audio and
    # the caller field still reflect the completed CAN 0 call.
    for seen in (72, 84):
        for _ in range(75):
            if vm.value('m17_seen') >= seen:
                break
            vm.wait_ms(40)
        else:
            raise AssertionError('The receiver missed the CAN 1 test call')
        assert vm.value('m17_seen_can', 1) == 1
        assert vm.value('m17_frames') == 120
        caller = vm.rsp.mem(vm.syms['m17_received_caller'], 10).split(b'\0', 1)[0]
        assert caller == b'W1CALL', caller


def main():
    try:
        from firmware_vm import ROOT, VM
    except ModuleNotFoundError as error:
        if error.name != 'firmware_vm':
            raise
        raise SystemExit('This regression requires tools/firmware_vm.py from the local DM32 emulator workspace.') from None

    with TemporaryDirectory() as temp:
        wave = Path(temp) / 'relayed-then-direct.s16le'
        subprocess.run([ROOT / 'build/test-link', wave], check=True)
        vm = VM('firmware-extended-callsign', rx_file=wave)
        try:
            vm.select()
            expect_label(vm, b'M0HOT', range(4, 72))
            expect_label(vm, b'W1CALL', range(72, 120))
            expect_label(vm, b'Last heard W1CALL', range(120, 121))
            assert vm.value('m17_rx_ended', 1)
            expect_filtered_call(vm)
            expect_label(vm, b'N0DIR', range(124, 168))
            expect_label(vm, b'Last heard N0DIR', range(168, 169))
            assert vm.value('m17_rx_ended', 1)
            assert not vm.value('m17_overruns')
            assert not vm.value('m17_underruns')
        finally:
            vm.close()
    print('PASS Home screen shows the relayed caller, keeps it through a filtered call, then shows the direct caller')


if __name__ == '__main__':
    main()

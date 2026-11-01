"""Reset guardrails, USB loss, fresh-boot verification and watchdog instruction."""
import hashlib
from pathlib import Path
import runpy
import struct
import subprocess
import unittest
from unittest.mock import MagicMock, patch

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_MEM_WRITE
import device_reboot as reboot

ROOT = Path(__file__).resolve().parents[1]
HANDLER = (ROOT / 'system/firmware_publisher/build/scsi-stub.bin').read_bytes()


class Device:
    def __init__(self, resets=True):
        self.report = bytearray(512)
        struct.pack_into('<I', self.report, 0x2c, 0x32505452)
        struct.pack_into('<3I', self.report, 0x50, 1, 1, 50)
        struct.pack_into('<I', self.report, 0x78, 3)
        self.resets, self.executed, self.ejected = resets, 0, False
        self.prepared = False
        self.finished = False
        self.before = bytes(self.report)

    def prepare_restart(self):
        self.prepared = True

    def peek(self, address, size):
        if address == 0x0819c1d4:
            return HANDLER
        if self.executed and self.resets:
            self.report[reboot.SCRATCH:] = bytes(512 - reboot.SCRATCH)
        offset = address - reboot.REPORT
        return self.report[offset:offset + size]

    def peek_ejected(self, address, size):
        assert self.ejected
        return self.peek(address, size)

    def scsi_write_file(self, path, address):
        assert self.prepared and address == reboot.REPORT
        self.report[:] = Path(path).read_bytes()
        assert len(self.report) == 512

    def scsi_exec(self, address):
        assert address == (reboot.REPORT + reboot.SCRATCH) | 1
        self.executed += 1
        raise subprocess.CalledProcessError(3, 'USB disconnected')

    def reconnect_after_restart(self):
        return True

    def finish_restart(self):
        self.ejected = True
        self.finished = True

    def eject(self):
        self.ejected = True


class RebootTests(unittest.TestCase):
    def run_restart(self, device):
        ticks = iter(range(20))
        with patch.object(reboot.time, 'sleep'), patch.object(
                reboot.time, 'monotonic', side_effect=lambda: next(ticks)):
            return reboot.restart(device, timeout=3)

    def test_pinned_handler_matches_compiled_source(self):
        self.assertEqual(hashlib.sha256(HANDLER).hexdigest(), reboot.HANDLER_SHA256)

    def test_disconnect_then_fresh_boot_is_success(self):
        d = Device()
        report = self.run_restart(d)
        self.assertEqual(d.executed, 1)
        self.assertTrue(d.ejected)
        self.assertEqual(report, d.before)
        self.assertTrue(d.finished)

    def test_valid_boot_of_wrong_resident_is_not_reported_as_update_success(self):
        d = Device()
        with patch.object(reboot.time, 'sleep'):
            with self.assertRaisesRegex(RuntimeError, 'different resident version'):
                reboot.restart(d, expected_version=123)
        self.assertTrue(d.ejected)

    def test_disconnect_alone_does_not_count_as_a_restart_or_retry_exec(self):
        d = Device(resets=False)
        with self.assertRaisesRegex(RuntimeError, 'fresh NanoApps boot'):
            self.run_restart(d)
        self.assertEqual(d.executed, 1)
        self.assertTrue(d.ejected)
        self.assertEqual(d.report[:reboot.SCRATCH], d.before[:reboot.SCRATCH])
        self.assertEqual(d.report[reboot.SCRATCH:reboot.SCRATCH + len(reboot.PAYLOAD)],
                         reboot.PAYLOAD)

    def test_bad_handler_report_and_occupied_scratch_refuse_execution(self):
        for failure in ('handler', 'report', 'scratch', 'readback'):
            with self.subTest(failure=failure):
                d = Device()
                if failure == 'report':
                    d.report[0x2c:0x30] = bytes(4)
                elif failure == 'scratch':
                    d.report[-1] = 1
                original = d.peek

                def peek(address, size):
                    if failure == 'handler' and address == 0x0819c1d4:
                        return bytes(140)
                    if failure == 'readback' and address == reboot.REPORT + reboot.SCRATCH:
                        return bytes(size)
                    return original(address, size)

                d.peek = peek
                with self.assertRaises(RuntimeError):
                    self.run_restart(d)
                self.assertEqual(d.executed, 0)

    def test_payload_only_writes_the_watchdog_reset_register(self):
        uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        uc.mem_map(0x1000, 4096)
        uc.mem_map(0x3c800000, 4096)
        uc.mem_write(0x1000, reboot.PAYLOAD)
        writes = []

        def write(u, access, address, size, value, _):
            writes.append((address, size, value))
            u.emu_stop()

        uc.hook_add(UC_HOOK_MEM_WRITE, write)
        uc.emu_start(0x1001, 0x2000, count=20)
        self.assertEqual(writes, [(0x3c800000, 4, 0x100000)])

    def test_mac_reconnect_uses_uuid_and_new_disk_node(self):
        ns = runpy.run_path(str(ROOT / 'start'))
        tr = ns['Transport']('local', None)
        tr.darwin = True
        tr._reboot_uuid = 'our-ipod'
        tr._dev = '/dev/disk32'
        ns = tr.reconnect_after_restart.__func__.__globals__
        with patch.dict(ns, mac_devices=lambda: [(
                {'DeviceIdentifier': 'disk4'}, {'VolumeUUID': 'another-disk'})]):
            self.assertFalse(tr.reconnect_after_restart())
        with patch.dict(ns, mac_devices=lambda: [(
                {'DeviceIdentifier': 'disk33'}, {'VolumeUUID': 'our-ipod'})]):
            with patch.dict(ns, MacDevice=lambda root, disk: disk):
                self.assertTrue(tr.reconnect_after_restart())
                self.assertEqual(tr._mac, '/dev/disk33')

    def test_mac_prepare_commits_media_before_reset_and_checks_returned_identity(self):
        prepare = runpy.run_path(str(ROOT / 'start'))['Transport'].prepare_restart
        for returned_uuid in ('our-ipod', 'other-ipod'):
            with self.subTest(returned_uuid=returned_uuid):
                events = []
                old = MagicMock(uuid='our-ipod', disk='/dev/disk22')
                old.unmount.side_effect = lambda: events.append('unmount-old')
                old.eject.side_effect = lambda: events.append('eject-old')
                new = MagicMock(uuid=returned_uuid, disk='/dev/disk23')
                new.unmount.side_effect = lambda: events.append('unmount-new')
                cls = prepare.__globals__['Transport']
                tr = cls('local', None)
                tr.darwin, tr._mac = True, old

                def load(_root):
                    events.append('load-media')
                    return new

                with patch.dict(prepare.__globals__, MacDevice=load), patch.object(
                        subprocess, 'run', side_effect=lambda *a, **kw: events.append('sync')):
                    if returned_uuid == 'our-ipod':
                        tr.prepare_restart()
                        self.assertEqual(tr._dev, '/dev/disk23')
                        self.assertEqual(events, ['sync', 'unmount-old', 'eject-old',
                                                  'load-media', 'unmount-new'])
                    else:
                        with self.assertRaisesRegex(RuntimeError, 'identity changed'):
                            tr.prepare_restart()
                        self.assertEqual(events, ['sync', 'unmount-old', 'eject-old', 'load-media'])

    def test_mac_unmount_retries_transient_finder_busy_without_forcing(self):
        from macos_transport import MacDevice
        m = object.__new__(MacDevice)
        m.disk = '/dev/disk22'
        busy = subprocess.CompletedProcess(['diskutil'], 1, 'could not be unmounted', '')
        good = subprocess.CompletedProcess(['diskutil'], 0, '', '')
        with patch.object(subprocess, 'run', side_effect=[busy, good]) as call, patch(
                'macos_transport.time.sleep'):
            m.unmount()
            self.assertEqual(call.call_count, 2)
            self.assertEqual(call.call_args.args[0], ['diskutil', 'unmountDisk', '/dev/disk22'])
        error = subprocess.CompletedProcess(['diskutil'], 1, '', 'No such disk')
        with patch.object(subprocess, 'run', return_value=error) as call:
            with self.assertRaises(subprocess.CalledProcessError):
                m.eject()
            self.assertEqual(call.call_count, 1)

    def test_linux_reconnect_uses_uuid_and_new_parent_disk(self):
        ns = runpy.run_path(str(ROOT / 'start'))
        tr = ns['Transport']('local', None)
        tr.darwin = False
        tr._reboot_uuid = 'our-ipod'
        with patch.object(subprocess, 'check_output', side_effect=[
                '/dev/sda1 another-disk\n/dev/sdc1 our-ipod\n', 'sdc\n']):
            self.assertTrue(tr.reconnect_after_restart())
            self.assertEqual(tr._dev, '/dev/sdc')
        with patch.object(subprocess, 'check_output', return_value='/dev/sda1 another-disk\n'):
            self.assertFalse(tr.reconnect_after_restart())

    def test_failed_file_install_never_restarts(self):
        ns = runpy.run_path(str(ROOT / 'start'))
        deploy = ns['_deploy_apps']
        tr = MagicMock()
        with patch.dict(deploy.__globals__, _install_disk=MagicMock(
                side_effect=OSError('file verification failed'))):
            with patch.object(subprocess, 'run'):
                with self.assertRaises(OSError):
                    deploy(tr, ['screenshot'], False, False, False, True)
        tr.restart.assert_not_called()

    def test_app_only_install_ejects_live_runtime_and_restarts_older_runtime(self):
        deploy = runpy.run_path(str(ROOT / 'start'))['_deploy_apps']
        for live in (False, True):
            with self.subTest(live=live):
                tr = MagicMock()
                tr.peek.return_value = struct.pack('<I', 0x4c495645 if live else 0)
                with patch.dict(deploy.__globals__, _install_disk=MagicMock()), patch.object(subprocess, 'run'):
                    deploy(tr, ['screenshot'], False, False, False, True)
                self.assertEqual(tr.eject.called, live)
                self.assertEqual(tr.restart.called, not live)


if __name__ == '__main__':
    unittest.main(verbosity=2)

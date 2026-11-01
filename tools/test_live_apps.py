"""Live Home reconciliation, stale taps, and bounded registrar allocations."""
import struct
import unittest

from app_install import pack, bundles, identity
from test_app_packages import app_files, EXE, ICON
from test_untether import ChainMachine, catalog_bundle, load_hrl
from unicorn.arm_const import UC_ARM_REG_R0
from unicorn.arm_const import UC_ARM_REG_SP


class LiveAppTests(unittest.TestCase):
    def boot(self):
        m = ChainMachine(real_resident=True)
        m.auto_register = True
        m.files['/Library/AllApps.pack'] = pack([])
        m.files.update(app_files('hb.first', 0x10000001))
        m.arm(); m.dispatch()
        self.assertEqual([identity(b) for b in bundles(m.registered_catalog)], ['hb.first'])
        return m

    def refresh(self, m, main=False):
        return m.call(m.res_addr('hb_refresh_apps'), (int(main),))

    def test_add_remove_and_incompatible_update_without_restart(self):
        m = self.boot()
        m.files.update(app_files('hb.second', 0x10000002))
        self.assertEqual(self.refresh(m, main=True), 1)
        self.assertEqual([identity(b) for b in bundles(m.registered_catalog)], ['hb.first', 'hb.second'])
        m.files.pop('/Apps/First.nanoapp/manifest.napp')
        self.assertEqual(self.refresh(m), 1)
        self.assertEqual(m.hidden_apps, ['hb.first'])
        self.assertEqual([identity(b) for b in bundles(m.registered_catalog)], ['hb.second'])
        key = '/Apps/Second.nanoapp/manifest.napp'
        manifest = bytearray(m.files[key])
        struct.pack_into('<I', manifest, 8, 999)  # unsupported app ABI
        m.files[key] = bytes(manifest)
        self.assertEqual(self.refresh(m), 1)
        self.assertEqual(m.hidden_apps, ['hb.first', 'hb.second'])
        self.assertEqual(bundles(m.registered_catalog), [])

    def test_missing_executable_removes_icon_even_when_code_was_cached(self):
        m = self.boot()
        slot = m.res_addr('s_app_entry')
        arena = m.heap_next
        m.heap_next += 0x1000
        m.live_allocs[arena] = 512
        m.word(slot, arena | 1)
        m.word(m.res_addr('s_app_arena'), arena)
        m.files.pop('/Apps/First.nanoapp/app.hbapp')
        self.assertEqual(self.refresh(m), 1)
        self.assertEqual(m.word(slot), 0)
        self.assertNotIn(arena, m.live_allocs)
        self.assertEqual(m.hidden_apps, ['hb.first'])

    def test_stopped_or_previous_view_timer_cannot_rearm_destroyed_timer(self):
        m = self.boot()
        view, msg = 0x09c00000, 0x09d00000
        m.word(msg + 4, 16)
        calls = []
        for address in (0x08423b44, 0x0842484c):
            def handler(uc, address=address):
                calls.append(address)
                m.ret()
            m.extra_handlers[address] = handler
        for magic, current, expected in ((0, view, []), (0x54494d52, view + 4, []),
                                        (0x54494d52, view, [0x08423b44, 0x0842484c])):
            m.word(m.res_addr('s_timer_magic'), magic)
            m.word(m.res_addr('s_surface_view'), current)
            calls.clear()
            self.assertEqual(m.call(m.res_addr('hb_msg_hook'), (view, msg)), 1)
            self.assertEqual(calls, expected)

    def test_not_ready_or_active_app_defers_without_mutating_home(self):
        m = self.boot()
        m.files.pop('/iPod_Control')
        self.assertEqual(self.refresh(m), 0)
        m.files['/iPod_Control'] = b''
        m.word(m.res_addr('s_cur_app_id'), 1)
        self.assertEqual(self.refresh(m), 0)
        self.assertEqual(m.hidden_apps, [])

    def test_repeated_refreshes_keep_only_two_catalogs(self):
        m = self.boot()
        self.assertEqual(self.refresh(m, main=True), 1)
        for i in range(12):
            if i % 2:
                m.files.pop('/Apps/Second.nanoapp/manifest.napp')
            else:
                m.files.update(app_files('hb.second', 0x10000002))
            self.assertEqual(self.refresh(m), 1)
        # resident, current catalog and one retired catalog; no app code loaded.
        self.assertEqual(len(m.live_allocs), 3)

    def test_main_screen_action_refreshes_after_usb_return(self):
        m = self.boot()
        m.files.update(app_files('hb.second', 0x10000002))
        name = 0x09d00000
        m.uc.mem_write(name, b'HandleGotoMainScreen\0')
        m.call(m.res_addr('action_hook'), (0x1111, name, 0))
        self.assertEqual(len(bundles(m.registered_catalog)), 1)
        self.fire_refresh(m)
        self.assertEqual(len(bundles(m.registered_catalog)), 2)

    def fire_refresh(self, m):
        msg = 0x09d00200
        m.word(msg + 4, 16)
        m.word(msg + 8, m.res_addr('s_refresh_timer'))
        return m.call(m.res_addr('hb_home_message_hook'), (0x1111, msg))

    def home_notice(self, m, code=0x6385, message_type=30):
        message, payload = 0x09d00000, 0x09d00100
        m.uc.mem_write(message, struct.pack('<4I', 0, message_type, payload, 0))
        m.uc.mem_write(payload, struct.pack('<4I', 0, code, 0, 0))
        return m.call(m.res_addr('rawmsg_hook'), (0x1111, message))

    def test_usb_model_notice_adds_and_removes_without_main_action_or_tap(self):
        m = self.boot()
        m.rawmsg_result = 7
        m.files.update(app_files('hb.second', 0x10000002))
        self.assertEqual(self.home_notice(m), 7)
        self.assertEqual(len(bundles(m.registered_catalog)), 1)
        self.fire_refresh(m)
        self.assertEqual(len(bundles(m.registered_catalog)), 2)
        m.files.pop('/Apps/First.nanoapp/manifest.napp')
        self.assertEqual(self.home_notice(m), 7)
        self.fire_refresh(m)
        self.assertEqual([identity(b) for b in bundles(m.registered_catalog)], ['hb.second'])
        for code, message_type in ((0x6384, 30), (0x6385, 15)):
            m.files.pop('/Apps/Second.nanoapp/manifest.napp', None)
            self.home_notice(m, code, message_type)
            self.assertEqual(len(bundles(m.registered_catalog)), 1)

    def test_usb_notices_coalesce_until_the_ui_timer_and_duplicate_ticks_are_ignored(self):
        m = self.boot()
        m.files.update(app_files('hb.second', 0x10000002))
        before = [c for c in m.calls if c[0] in ('size', 'read', 'exists')]
        self.home_notice(m)
        self.home_notice(m)
        self.assertEqual([c for c in m.calls if c[0] in ('size', 'read', 'exists')], before)
        pending = m.res_base + m.res_syms['s_refresh_pending']
        self.assertEqual(bytes(m.uc.mem_read(pending, 1)), b'\x01')
        self.fire_refresh(m)
        self.assertEqual(len(bundles(m.registered_catalog)), 2)
        m.files.pop('/Apps/Second.nanoapp/manifest.napp')
        self.fire_refresh(m)
        self.assertEqual(len(bundles(m.registered_catalog)), 2)

    def test_first_usb_eject_loads_apps_after_boot_with_unavailable_disk(self):
        m = ChainMachine(real_resident=True)
        m.auto_register = True
        m.files.pop('/Library/AllApps.pack', None)
        m.files.pop('/iPod_Control')
        # Enter as the resource selector does. The historical bootstrap used by
        # the other fixtures deliberately requires an AllApps.pack sentinel.
        _, span, _ = load_hrl(m.uc, m.res_blob, m.res_base)
        m.heap_next = (m.res_base + span + 0xfff) & ~0xfff
        m.call(m.res_addr('payload_entry'), (m.res_base, span, 100))
        m.call(m.res_addr('action_hook'), (0x1111, 0, 0))
        self.assertEqual(bundles(m.registered_catalog), [])
        m.files['/iPod_Control'] = b''
        m.files.update(app_files('hb.first', 0x10000001))
        self.home_notice(m)
        self.fire_refresh(m)
        self.assertEqual([identity(b) for b in bundles(m.registered_catalog)], ['hb.first'])

    def test_usb_notice_finishes_stock_transition_before_package_scan(self):
        m = self.boot()
        m.files.pop('/iPod_Control')
        def stock_transition(uc):
            m.files['/iPod_Control'] = b''
            m.files.update(app_files('hb.second', 0x10000002))
            # The OS may release payload data before returning.
            m.uc.mem_write(0x09d00100, bytes(16))
            m.ret(9)
        m.extra_handlers[m.res_addr('hb_silver_rawmsg') & ~1] = stock_transition
        self.assertEqual(self.home_notice(m), 9)
        self.assertEqual(len(bundles(m.registered_catalog)), 1)
        self.fire_refresh(m)
        self.assertEqual(len(bundles(m.registered_catalog)), 2)

    def test_renamed_folder_refresh_keeps_identity_and_launches_from_actual_path(self):
        m = self.boot()
        for path in list(m.files):
            if path.startswith('/Apps/First.nanoapp/'):
                m.files[path.replace('First.nanoapp', 'My First App.nanoapp')] = m.files.pop(path)
        self.assertEqual(self.refresh(m, main=True), 1)
        self.assertEqual([identity(b) for b in bundles(m.registered_catalog)], ['hb.first'])
        paths = []
        def load_executable(uc):
            paths.append(m.string(uc.reg_read(UC_ARM_REG_R0)))
            m.ret(0)  # stop before screen setup; only the launch path is under test
        m.extra_handlers[m.res_addr('hb_reloc_load_file_verified') & ~1] = load_executable
        m.call(m.res_addr('hb_app_launch'), (0x1111, m.res_addr('g_launch')))
        self.assertEqual(paths, ['/Apps/My First App.nanoapp/app.hbapp'])

    def test_icon_resolver_reads_actual_folder_and_checks_integrity(self):
        m = self.boot()
        state = m.res_addr('s_icon_state')
        names, buffer, text, output = 0x09c00000, 0x09c10000, 0x09d00000, 0x09d00100
        m.word(state + 8, 1000)  # first cache ID
        m.word(state + 12, 1)
        m.word(state + 20, names)
        m.word(state + 344, 0xffffffff)  # first LRU entry: ID + buffer
        m.word(state + 348, buffer)
        m.uc.mem_write(text, b'My First App.nanoapp\0')
        m.call(m.res_addr('hb_silver_icon_set_name'), (0, text))
        from wrap_resident_slot import fnv1a32
        m.call(m.res_addr('hb_silver_icon_set_hash'), (0, fnv1a32(ICON), 1))
        m.intercept(0x0841abd2, 'zero')  # stock bitmap refcount
        m.intercept(0x0801b0e8, 'zero')  # stock resolver fallback
        path = '/Apps/My First App.nanoapp/icon.bin'
        m.files[path] = ICON
        self.assertEqual(m.call(m.res_addr('icon_lazy_resolver'), (1000, output)), 1000)
        self.assertEqual(m.word(output), buffer)
        self.assertIn(('read', path), m.calls)
        m.word(state + 344, 0xffffffff)
        m.word(state + 16, 0)  # revisit the same LRU buffer
        m.files[path] = ICON[:-1] + b'\x01'
        self.assertEqual(m.call(m.res_addr('icon_lazy_resolver'), (1000, output)), 0)

    def test_stale_icon_tap_reconciles_before_routing(self):
        m = self.boot()
        m.files.pop('/Apps/First.nanoapp/app.hbapp')
        def raw_message(uc):
            output = m.word(uc.reg_read(UC_ARM_REG_SP))
            uc.mem_write(output, b'sbitem.hb.first.clicked\0')
            m.ret(2)
        m.extra_handlers[m.res_addr('hb_silver_rawmsg') & ~1] = raw_message
        self.assertEqual(m.call(m.res_addr('rawmsg_hook'), (0x1111, 0)), 1)
        self.assertEqual(m.hidden_apps, ['hb.first'])
        self.assertEqual(m.word(m.res_addr('g_launch_count')), 0)
        self.assertEqual(m.word(m.res_addr('s_app_entry')), 0)

    def test_executable_checksum_is_checked_before_publishing_code(self):
        m = self.boot()
        path, out = 0x09d00000, 0x09d00100
        m.uc.mem_write(path, b'/Apps/First.nanoapp/app.hbapp\0')
        from wrap_resident_slot import fnv1a32
        args = (path, out, out + 4, fnv1a32(EXE))
        self.assertNotEqual(m.call(m.res_addr('hb_reloc_load_file_verified'), args), 0)
        corrupted = EXE[:-1] + bytes([EXE[-1] ^ 1])
        m.files['/Apps/First.nanoapp/app.hbapp'] = corrupted
        flushes = len(m.flushes)
        self.assertEqual(m.call(m.res_addr('hb_reloc_load_file_verified'), args), 0)
        self.assertEqual(len(m.flushes), flushes)


class RegistrarTests(unittest.TestCase):
    def test_refresh_reuses_icon_extension_and_label_objects(self):
        m = ChainMachine(real_resident=True)
        _, span, _ = load_hrl(m.uc, m.res_blob, m.res_base)
        m.heap_next = (m.res_base + span + 0xfff) & ~0xfff
        m.handlers.pop(m.res_addr('hb_app_register_pack') & ~1)
        calls = []
        methods = {
            'hb_silver_icon_grow_cache': 0, 'hb_silver_icon_register_lazy': 0x1000,
            'hb_silver_icon_refresh': 0x1000, 'hb_silver_home_ensure_capacity': 0,
            'hb_silver_home_trim_pages': 0, 'hb_silver_home_reload': 0,
            'hb_silver_icon_set_name': 0, 'hb_silver_icon_set_hash': 0,
            'hb_silver_home_add_item': 0, 'hb_silver_resdb_has_tag': 0,
            'hb_silver_resdb_unpublish': 0, 'hb_silver_resdb_publish': 1,
            'hb_silver_resdb_open': None,
        }
        for name, result in methods.items():
            def handler(uc, name=name, result=result):
                r0 = uc.reg_read(UC_ARM_REG_R0)
                calls.append((name, r0))
                m.ret(r0 if result is None else result)
            m.extra_handlers[m.res_addr(name) & ~1] = handler
        address, output = 0x09c00000, 0x09c10000
        data = pack([catalog_bundle()])
        m.uc.mem_write(address, data)
        args = (address, m.res_addr('hb_os_alloc') | 1, 0, output, 64, 0, 0)
        m.call(m.res_addr('hb_app_register_pack'), (*args, 1, 0))
        allocations = dict(m.live_allocs)
        for _ in range(10):
            m.call(m.res_addr('hb_app_register_pack'), (*args, 2, 0))
        self.assertEqual(m.live_allocs, allocations)
        self.assertEqual([x for x in calls if x[0] == 'hb_silver_icon_register_lazy'],
                         [('hb_silver_icon_register_lazy', 64)])
        self.assertEqual(sum(x[0] == 'hb_silver_icon_refresh' for x in calls), 10)


if __name__ == '__main__':
    unittest.main(verbosity=2)

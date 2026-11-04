"""실제 VRM C 함수를 호스트 모의 장치에 연결해 복원 실패 경로를 검증한다."""

from pathlib import Path
import os
import re
import resource
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT


def c_function(source, name):
    """시험용으로 실제 함수 본문을 그대로 가져오며 별도 구현을 만들지 않는다."""
    match = re.search(r"^static s32 " + re.escape(name) + r"\([^;]*?\)\n\{", source, re.M)
    if not match:
        raise AssertionError(f"검증할 함수가 없습니다: {name}")
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
struct kernel_param { int unused; };
struct kernel_param_ops {
    int (*set)(const char *, const struct kernel_param *);
    int (*get)(char *, const struct kernel_param *);
};
static int param_get_int(char *p, const struct kernel_param *k) { return 0; }
#define module_param_cb(...)
#define MODULE_PARM_DESC(...)
#define pr_info(...) ((void)0)
#define pr_err(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define I2C_SMBUS_WRITE 0
#define I2C_SMBUS_READ 1
#define I2C_SMBUS_QUICK 0
#define I2C_SMBUS_BYTE 1
#define I2C_SMBUS_BYTE_DATA 2
#define I2C_SMBUS_WORD_DATA 3
#define I2C_LOCK_SEGMENT 2
union i2c_smbus_data { u8 byte; u16 word; };
static int sleeps;
static void usleep_range(unsigned a, unsigned b) { sleeps++; }
'''


PAGE_STUB = r'''
struct i2c_adapter { int unused; };
struct device { int unused; };
struct i2c_client {
    struct i2c_adapter *adapter;
    u16 addr;
    unsigned short flags;
    struct device dev;
};
static int lock_depth, locks, unlocks, transfers, reads, writes;
static int wire_page = 1;
static int save_error, select_error, select_read_error, sensor_error;
static int restore_error, restore_read_error;
static bool select_drop, restore_drop, select_changes_on_error;
static bool restore_changes_on_error;
static void i2c_lock_bus(struct i2c_adapter *a, unsigned flags) {
    assert(flags == I2C_LOCK_SEGMENT);
    assert(lock_depth == 0);
    lock_depth++; locks++;
}
static void i2c_unlock_bus(struct i2c_adapter *a, unsigned flags) {
    assert(flags == I2C_LOCK_SEGMENT);
    assert(lock_depth == 1);
    lock_depth--; unlocks++;
}
static s32 __i2c_smbus_xfer(struct i2c_adapter *a, u16 addr,
        unsigned short flags, char direction, u8 reg, int size,
        union i2c_smbus_data *value) {
    assert(lock_depth == 1);
    assert(addr == 0x60);
    transfers++;
    if (reg == 0 && direction == I2C_SMBUS_READ) {
        reads++;
        if (reads == 1 && save_error) return save_error;
        if (writes == 1 && select_read_error) return select_read_error;
        if (writes == 2 && restore_read_error) return restore_read_error;
        value->byte = wire_page;
        return 0;
    }
    if (reg == 0 && direction == I2C_SMBUS_WRITE) {
        writes++;
        if (writes == 1) {
            if (!select_drop && (!select_error || select_changes_on_error))
                wire_page = value->byte;
            return select_error;
        }
        assert(writes == 2);
        if (!restore_drop && (!restore_error || restore_changes_on_error))
            wire_page = value->byte;
        return restore_error;
    }
    assert(direction == I2C_SMBUS_READ && size == I2C_SMBUS_WORD_DATA);
    assert(reg == 0x8d);
    if (sensor_error) return sensor_error;
    value->word = 55;
    return 0;
}
'''


PAGE_MAIN = r'''
int main(int argc, char **argv) {
    struct i2c_adapter adapter = {0};
    struct i2c_client client = { .adapter = &adapter, .addr = 0x60 };
    int ret, before, expected = 55;
    const char *test = argv[1];
    assert(argc == 2);
    assert(bc250_page_diag_ops.set == NULL);
    if (!strcmp(test, "save_error")) { save_error = -EIO; expected = -EIO; }
    if (!strcmp(test, "select_timeout_changed")) {
        select_error = -ETIMEDOUT;
        select_changes_on_error = true;
        expected = -ETIMEDOUT;
    }
    if (!strcmp(test, "select_timeout_unchanged")) {
        select_error = -ETIMEDOUT;
        expected = -ETIMEDOUT;
    }
    if (!strcmp(test, "select_read_error")) { select_read_error = -EIO; expected = -EIO; }
    if (!strcmp(test, "select_not_applied")) { select_drop = true; expected = -EIO; }
    if (!strcmp(test, "sensor_error")) { sensor_error = -ENXIO; expected = -ENXIO; }
    if (!strcmp(test, "restore_timeout_verified")) {
        restore_error = -ETIMEDOUT;
        restore_changes_on_error = true;
    }
    if (!strcmp(test, "restore_timeout_unverified")) {
        restore_error = -ETIMEDOUT;
        expected = -ETIMEDOUT;
    }
    if (!strcmp(test, "restore_not_applied")) { restore_drop = true; expected = -EIO; }
    if (!strcmp(test, "restore_read_error")) { restore_read_error = -ENXIO; expected = -ENXIO; }
    if (!strcmp(test, "same_page")) wire_page = 0;
    ret = bc250_vrm_read_word(&client, 0, 0x8d);
    assert(ret == expected);
    assert(lock_depth == 0 && locks == 1 && unlocks == 1);
    if (!strcmp(test, "save_error")) {
        assert(writes == 0 && transfers == 1 && initial_page == -1);
    } else if (!strcmp(test, "same_page")) {
        assert(writes == 0 && transfers == 2 && initial_page == 0);
    } else {
        assert(writes == 2 && initial_page == 1);
        if (!strcmp(test, "restore_timeout_unverified") ||
                !strcmp(test, "restore_not_applied") ||
                !strcmp(test, "restore_read_error")) {
            assert(page_restore_error == expected && restore_page == 1);
            before = transfers;
            ret = bc250_vrm_read_word(&client, 1, 0x8d);
            assert(ret == expected && transfers == before && locks == 1);
        } else {
            assert(wire_page == 1 && page_restore_error == 0 && restore_page == -1);
        }
    }
    if (!strcmp(test, "preserve_initial_page")) {
        /* 다른 소비자가 바꾼 원 PAGE와 최초 관측 PAGE를 구별한다. */
        wire_page = 3; reads = writes = 0; restore_drop = true;
        ret = bc250_vrm_read_word(&client, 0, 0x8d);
        assert(ret == -EIO && initial_page == 1 && restore_page == 3);
        before = transfers;
        assert(bc250_vrm_read_word(&client, 0, 0x8d) == -EIO);
        assert(transfers == before);
    }
    puts("통과");
    return 0;
}
'''


BUS_STUB = r'''
#define DEFINE_MUTEX(x) int x
#define mutex_lock(p) ((void)0)
#define mutex_unlock(p) ((void)0)
#define THIS_MODULE NULL
#define GFP_KERNEL 0
#define I2C_FUNC_SMBUS_BYTE_DATA 1
#define I2C_FUNC_SMBUS_WORD_DATA 2
#define I2C_FUNC_SMBUS_BYTE 4
#define I2C_FUNC_SMBUS_QUICK 8
#define IORESOURCE_IO 1
struct resource { const char *name; unsigned long flags, start, end; };
struct i2c_adapter;
struct i2c_algorithm {
    s32 (*smbus_xfer)(struct i2c_adapter *, u16, unsigned short, char,
                      u8, int, union i2c_smbus_data *);
    u32 (*functionality)(struct i2c_adapter *);
};
struct i2c_adapter {
    void *owner;
    unsigned int class;
    const struct i2c_algorithm *algo;
    char name[48];
};
static u8 registers[256][256], sio[256], selected_sio, selected_page, selected_index;
static int ec_writes, cfg_writes, commands, resets, released;
static int busy_cycles, busy_remaining, add_error;
static bool initial_busy, reset_stuck, reset_has_happened, transaction_active;
static bool drop_apply, drop_restore, allocation_fails, region_conflict;
static bool sio_base_changes, acpi_conflict;
static unsigned base_read_count;
static u8 inb(unsigned port) {
    if (port == 0x2f) {
        if (selected_sio == 0x61 && sio_base_changes && ++base_read_count == 2)
            return sio[selected_sio] ^ 8;
        return sio[selected_sio];
    }
    assert(port == 0xa22);
    if (selected_page == 4 && selected_index == 0x60) {
        if (initial_busy || (reset_has_happened && reset_stuck)) return 0x40;
        if (transaction_active) {
            if (busy_remaining-- > 0) return 0x40;
            return 0x80;
        }
        return 0;
    }
    return registers[selected_page][selected_index];
}
static void outb(u8 value, unsigned port) {
    if (port == 0x2e) { selected_sio = value; return; }
    if (port == 0x2f) { sio[selected_sio] = value; return; }
    if (port == 0xa20) { selected_page = value; return; }
    if (port == 0xa21) { selected_index = value; return; }
    assert(port == 0xa22);
    ec_writes++;
    if (selected_page == 4 && (selected_index == 0x61 || selected_index == 0x62)) {
        cfg_writes++;
        if (drop_apply && selected_index == 0x62 && value == 3) return;
        if (drop_restore && selected_index == 0x61 && value == 0x21) return;
    }
    if (selected_page == 4 && selected_index == 0x60) {
        if (value == 0xc0) {
            commands++; transaction_active = true; busy_remaining = busy_cycles;
        }
        if (value == 0x08) {
            resets++; reset_has_happened = true; transaction_active = false;
        }
    }
    registers[selected_page][selected_index] = value;
}
static void *request_muxed_region(unsigned a, unsigned n, const char *name) {
    return (void *)1;
}
static void *request_region(unsigned a, unsigned n, const char *name) {
    assert(a == 0xa20 && n == 4);
    return region_conflict ? NULL : (void *)1;
}
static void release_region(unsigned a, unsigned n) { if (a == 0xa20) released++; }
static int acpi_check_resource_conflict(struct resource *r) {
    assert(r->start == 0xa20 && r->end == 0xa23);
    return acpi_conflict ? -EBUSY : 0;
}
static void *kzalloc(size_t n, int flags) { return allocation_fails ? NULL : calloc(1, n); }
static void kfree(void *p) { free(p); }
static int i2c_add_adapter(struct i2c_adapter *a) { return add_error; }
static void i2c_del_adapter(struct i2c_adapter *a) {}
static size_t strscpy(char *to, const char *from, size_t n) {
    snprintf(to, n, "%s", from); return strlen(from);
}
'''


BUS_MAIN = r'''
int main(int argc, char **argv) {
    struct i2c_adapter *adapter = NULL;
    union i2c_smbus_data data = {0};
    u16 base;
    int ret, before;
    const char *test = argv[1];
    assert(argc == 2);
    assert(nct6687_diag_ops.set == NULL);
    sio[0x20] = 0xd4; sio[0x21] = 0x40; sio[0x30] = 1;
    sio[0x60] = 0x0a; sio[0x61] = 0x20;
    registers[4][0x61] = 0x21; registers[4][0x62] = 0x45;
    registers[4][0xb0] = 55;
    if (!strcmp(test, "base_rules")) {
        assert(nct6687_smbus_normalize_base(0xa20, &base) == 0 && base == 0xa20);
        assert(nct6687_smbus_normalize_base(0xa25, &base) == 0 && base == 0xa20);
        assert(nct6687_smbus_normalize_base(0xa23, &base) == -ENODEV);
        assert(nct6687_smbus_normalize_base(0xff8, &base) == 0 && base == 0xff8);
        assert(nct6687_smbus_normalize_base(0x1000, &base) == -ENODEV);
        assert(nct6687_smbus_normalize_base(0xffff, &base) == -ENODEV);
        assert(nct6687_smbus_normalize_base(0, &base) == -ENODEV);
        puts("통과"); return 0;
    }
    if (!strcmp(test, "initial_busy")) initial_busy = true;
    if (!strcmp(test, "base_unstable")) sio_base_changes = true;
    if (!strcmp(test, "base_unaligned")) sio[0x61] = 0x23;
    if (!strcmp(test, "base_normalized")) sio[0x61] = 0x25;
    if (!strcmp(test, "wrong_chip")) sio[0x20] = 0x12;
    if (!strcmp(test, "inactive")) sio[0x30] = 0;
    if (!strcmp(test, "region_conflict")) region_conflict = true;
    if (!strcmp(test, "acpi_conflict")) acpi_conflict = true;
    if (!strcmp(test, "apply_mismatch")) drop_apply = true;
    if (!strcmp(test, "allocation_failure")) allocation_fails = true;
    if (!strcmp(test, "adapter_failure")) add_error = -EINVAL;
    ret = nct6687_smbus_init(&adapter);
    if (initial_busy || sio_base_changes || sio[0x61] == 0x23 ||
            sio[0x20] != 0xd4 || !sio[0x30] || region_conflict || acpi_conflict) {
        assert(ret < 0 && adapter == NULL && cfg_writes == 0 && commands == 0);
        assert(saved_cfg1 == -1 && saved_cfg2 == -1);
        assert(config_restore_attempted == 0);
        puts("통과"); return 0;
    }
    if (drop_apply || allocation_fails || add_error) {
        assert(ret < 0 && adapter == NULL && released == 1);
        assert(config_restore_attempted == 1 && config_restore_error == 0);
        assert(registers[4][0x61] == 0x21 && registers[4][0x62] == 0x45);
        assert(saved_cfg1 == 0x21 && saved_cfg2 == 0x45);
        puts("통과"); return 0;
    }
    assert(ret == 0 && adapter != NULL && adapter->class == 0);
    assert(ec_base == 0xa20 && saved_cfg1 == 0x21 && saved_cfg2 == 0x45);
    assert(config_restore_attempted == 0);
    if (!strcmp(test, "transfer_busy")) initial_busy = true;
    if (!strcmp(test, "timeout_recovers") || !strcmp(test, "timeout_stuck")) busy_cycles = 1000;
    if (!strcmp(test, "timeout_stuck")) reset_stuck = true;
    if (!strcmp(test, "config_changed")) registers[4][0x61] = 9;
    before = ec_writes;
    ret = nct6687_smbus_xfer(adapter, 0x60, 0, I2C_SMBUS_READ,
                           0x98, I2C_SMBUS_BYTE_DATA, &data);
    if (!strcmp(test, "transfer_busy")) {
        assert(ret == -EBUSY && commands == 0 && resets == 0 && ec_writes == before);
        initial_busy = false;
    } else if (!strcmp(test, "timeout_recovers")) {
        assert(ret == -ETIMEDOUT && resets == 1 && transport_error == 0);
        busy_cycles = 0;
        assert(nct6687_smbus_xfer(adapter, 0x60, 0, I2C_SMBUS_READ,
                0x98, I2C_SMBUS_BYTE_DATA, &data) == 0);
    } else if (!strcmp(test, "timeout_stuck")) {
        assert(ret == -EBUSY && resets == 1 && transport_error == -EBUSY);
        before = ec_writes;
        assert(nct6687_smbus_xfer(adapter, 0x60, 0, I2C_SMBUS_READ,
                0x98, I2C_SMBUS_BYTE_DATA, &data) == -EBUSY);
        assert(ec_writes == before && commands == 1);
    } else if (!strcmp(test, "config_changed")) {
        assert(ret == -EIO && commands == 0 && transport_error == -EIO);
        assert(ec_writes == before);
    } else {
        assert(ret == 0 && data.byte == 55);
    }
    if (!strcmp(test, "restore_busy")) initial_busy = true;
    if (!strcmp(test, "restore_mismatch")) drop_restore = true;
    before = cfg_writes;
    nct6687_smbus_exit(adapter);
    assert(released == 1 && config_restore_attempted == 1);
    assert(saved_cfg1 == 0x21 && saved_cfg2 == 0x45);
    if (!strcmp(test, "timeout_stuck") || !strcmp(test, "restore_busy")) {
        assert(config_restore_error == -EBUSY && nct6687_config_saved);
        assert(cfg_writes == before);
    } else if (!strcmp(test, "restore_mismatch")) {
        assert(config_restore_error == -EIO && nct6687_config_saved);
    } else {
        assert(config_restore_error == 0 && !nct6687_config_saved);
        assert(registers[4][0x61] == 0x21 && registers[4][0x62] == 0x45);
    }
    puts("통과");
    return 0;
}
'''


def no_core_dump():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


class VrmDriverFailureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = shutil.which(os.environ.get("CC", "cc"))
        if not cls.cc:
            raise unittest.SkipTest("호스트 C 컴파일러가 없습니다")
        cls.directory = tempfile.TemporaryDirectory(prefix="bc250-vrm-")
        cls.work = Path(cls.directory.name)
        source = (DRIVER / "bc250_vrm_core.c").read_text()
        diagnostics = source[source.index("static int initial_page"):
                             source.index("static const char * const bc250_vrm_in_labels")]
        constants = "\n".join(line for line in source.splitlines() if line.startswith("#define"))
        cls.page_source = COMMON + PAGE_STUB + constants + "\n" + diagnostics + "\n" + "\n".join(
            c_function(source, name) for name in ["bc250_vrm_xfer_unlocked", "bc250_vrm_read_word"]
        ) + PAGE_MAIN
        bus = (DRIVER / "nct6687_smbus.c").read_text()
        bus = re.sub(r"^#include[^\n]*\n", "", bus, flags=re.M)
        cls.compile("page", cls.page_source)
        cls.compile("bus", COMMON + BUS_STUB + bus + BUS_MAIN)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    @classmethod
    def compile(cls, name, source):
        path = cls.work / f"{name}.c"
        path.write_text(source)
        result = subprocess.run([
            cls.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-unused-variable",
            "-Wno-unused-const-variable", "-o", str(cls.work / name), str(path),
        ], capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise AssertionError(f"C 모의 검증기 컴파일 실패:\n{result.stderr}")

    def run_case(self, binary, case, *, failure=False):
        result = subprocess.run([str(self.work / binary), case], capture_output=True,
                                text=True, timeout=5, cwd=self.work, preexec_fn=no_core_dump)
        if failure:
            self.assertNotEqual(result.returncode, 0, "잘못된 대조군을 검증기가 통과시켰습니다")
        else:
            self.assertEqual(result.returncode, 0, f"{case}: {result.stderr}")
            self.assertEqual(result.stdout.strip(), "통과")

    def test_page_transaction_failures(self):
        for case in ["normal", "same_page", "save_error", "select_timeout_changed",
                     "select_timeout_unchanged", "select_read_error", "select_not_applied",
                     "sensor_error", "restore_timeout_verified", "restore_timeout_unverified",
                     "restore_not_applied", "restore_read_error", "preserve_initial_page"]:
            with self.subTest(case=case):
                self.run_case("page", case)

    def test_controller_failures(self):
        for case in ["normal", "base_rules", "initial_busy", "base_unstable", "base_unaligned",
                     "base_normalized", "wrong_chip", "inactive", "region_conflict", "acpi_conflict",
                     "apply_mismatch", "allocation_failure", "adapter_failure", "transfer_busy",
                     "timeout_recovers", "timeout_stuck", "config_changed", "restore_busy",
                     "restore_mismatch"]:
            with self.subTest(case=case):
                self.run_case("bus", case)

    def test_negative_control_catches_missing_restore(self):
        original = "if (ret < 0)\n\t\t\tgoto restore;"
        self.assertIn(original, self.page_source)
        broken = self.page_source.replace(original, "if (ret < 0)\n\t\t\tgoto unlock_bus;", 1)
        self.compile("broken-page", broken)
        self.run_case("broken-page", "select_timeout_changed", failure=True)


if __name__ == "__main__":
    unittest.main()

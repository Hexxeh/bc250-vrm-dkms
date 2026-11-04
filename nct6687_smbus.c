// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * nct6687_smbus.c - Nuvoton NCT6686/NCT6687 Super I/O Hardware SMBus Master Driver
 *
 * Implements a Linux I2C/SMBus adapter driving the hardware SMBus Master block
 * on Nuvoton NCT6686 / NCT6687 series Super I/O chips via the on-chip Embedded
 * Controller (EC) Space Page 4 mailbox protocol.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/acpi.h>

#include "nct6687_smbus.h"

/* Super I/O Registers */
#define SIO_REG_PORT              0x2E
#define SIO_DATA_PORT             0x2F
#define SIO_LDN_SEL               0x07
#define SIO_LDN_EC                0x0B
#define SIO_REG_ACTIVATE          0x30
#define SIO_REG_ADDR_HIGH         0x60
#define SIO_REG_ADDR_LOW          0x61
#define SIO_REG_DEVID             0x20

/* Embedded Controller Space (Port 0) */
#define NCT6687_EC_BASE_DEFAULT   0x0A20
#define NCT6687_EC_PORT_LEN       4

#define EC_PAGE_REG_OFFSET        0
#define EC_INDEX_REG_OFFSET       1
#define EC_DATA_REG_OFFSET        2
#define EC_PAGE_SELECT            0xFF

/* EC Space Page 4: SMBus Master Registers */
#define EC_PAGE_SMBUS             4
#define EC_REG_STATUS             0x60
#define EC_REG_BUS_CFG1           0x61
#define EC_REG_BUS_CFG2           0x62
#define EC_REG_PROTOCOL           0x63
#define EC_REG_SLAVE_ADDR         0x65
#define EC_REG_CMD                0x66
#define EC_REG_DATA_IN0           0x70
#define EC_REG_DATA_IN1           0x71
#define EC_REG_DATA_OUT0          0xB0
#define EC_REG_DATA_OUT1          0xB1

#define EC_CMD_START              0xC0
#define EC_CMD_RESET              0x08
#define EC_STATUS_BUSY            0x40
#define EC_STATUS_SUCCESS         0x80

#define EC_PROTO_WRITE_BYTE       0x02
#define EC_PROTO_WRITE_WORD       0x03
#define EC_PROTO_READ_BYTE        0x82
#define EC_PROTO_READ_WORD        0x83

static DEFINE_MUTEX(nct6687_smbus_lock);
static u16 nct6687_ec_base = NCT6687_EC_BASE_DEFAULT;
static int saved_cfg1 = -1;
static int saved_cfg2 = -1;
static int ec_base = -1;
static int config_restore_attempted;
static int config_restore_error;
static int transport_error;
static bool nct6687_config_saved;

/* setter를 제공하지 않아 적재 인자로도 복원 기준값을 바꿀 수 없다. */
static const struct kernel_param_ops nct6687_diag_ops = {
	.get = param_get_int,
};
module_param_cb(saved_cfg1, &nct6687_diag_ops, &saved_cfg1, 0444);
module_param_cb(saved_cfg2, &nct6687_diag_ops, &saved_cfg2, 0444);
module_param_cb(ec_base, &nct6687_diag_ops, &ec_base, 0444);
module_param_cb(config_restore_attempted, &nct6687_diag_ops,
		&config_restore_attempted, 0444);
module_param_cb(config_restore_error, &nct6687_diag_ops, &config_restore_error, 0444);
module_param_cb(transport_error, &nct6687_diag_ops, &transport_error, 0444);
MODULE_PARM_DESC(saved_cfg1, "최초 EC SMBus CFG1 값, 아직 저장하지 않았으면 -1");
MODULE_PARM_DESC(saved_cfg2, "최초 EC SMBus CFG2 값, 아직 저장하지 않았으면 -1");
MODULE_PARM_DESC(ec_base, "실제 예약한 EC port0 기준 주소");
MODULE_PARM_DESC(config_restore_attempted, "CFG 복원 시도 여부");
MODULE_PARM_DESC(config_restore_error, "CFG 복원 결과, 시도 여부와 함께 판독");
MODULE_PARM_DESC(transport_error, "복구 불가능한 전송 오류, 오류 뒤 전송 중단");

static inline void nct6687_ec_write(u8 page, u8 index, u8 val)
{
	outb(EC_PAGE_SELECT, nct6687_ec_base + EC_PAGE_REG_OFFSET);
	outb(page, nct6687_ec_base + EC_PAGE_REG_OFFSET);
	outb(index, nct6687_ec_base + EC_INDEX_REG_OFFSET);
	outb(val, nct6687_ec_base + EC_DATA_REG_OFFSET);
	outb(EC_PAGE_SELECT, nct6687_ec_base + EC_PAGE_REG_OFFSET);
}

static inline u8 nct6687_ec_read(u8 page, u8 index)
{
	u8 val;
	outb(EC_PAGE_SELECT, nct6687_ec_base + EC_PAGE_REG_OFFSET);
	outb(page, nct6687_ec_base + EC_PAGE_REG_OFFSET);
	outb(index, nct6687_ec_base + EC_INDEX_REG_OFFSET);
	val = inb(nct6687_ec_base + EC_DATA_REG_OFFSET);
	outb(EC_PAGE_SELECT, nct6687_ec_base + EC_PAGE_REG_OFFSET);
	return val;
}

/* 진행 중인 외부 전송에는 RESET을 쓰지 않고 유휴가 될 때까지만 기다린다. */
static int nct6687_smbus_wait_idle(void)
{
	int tries;

	for (tries = 0; tries < 100; tries++) {
		if (!(nct6687_ec_read(EC_PAGE_SMBUS, EC_REG_STATUS) & EC_STATUS_BUSY))
			return 0;
		usleep_range(400, 600);
	}
	return -EBUSY;
}

/* 어댑터 해제 뒤 최초 설정을 복원하며 초기화 실패도 같은 경로를 쓴다. */
static int nct6687_smbus_restore_config(void)
{
	int actual_cfg1 = -1, actual_cfg2 = -1;
	int ret;

	if (!nct6687_config_saved)
		return 0;

	config_restore_attempted = 1;
	ret = nct6687_smbus_wait_idle();
	if (ret)
		goto report;

	nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_BUS_CFG1, saved_cfg1);
	nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_BUS_CFG2, saved_cfg2);
	actual_cfg1 = nct6687_ec_read(EC_PAGE_SMBUS, EC_REG_BUS_CFG1);
	actual_cfg2 = nct6687_ec_read(EC_PAGE_SMBUS, EC_REG_BUS_CFG2);
	if (actual_cfg1 != saved_cfg1 || actual_cfg2 != saved_cfg2)
		ret = -EIO;

report:
	config_restore_error = ret;
	if (ret) {
		/* 최초 값을 유지하며 모듈 제거 성공을 하드웨어 복원 성공으로 보지 않는다. */
		pr_err("nct6687_smbus: 최초 SMBus 설정 복원 확인 실패 config_restore_attempted=1 config_restore_error=%d ec_base=0x%04x saved_cfg1=%d saved_cfg2=%d actual_cfg1=%d actual_cfg2=%d\n",
			ret, nct6687_ec_base, saved_cfg1, saved_cfg2, actual_cfg1, actual_cfg2);
	} else {
		nct6687_config_saved = false;
		pr_info("nct6687_smbus: 최초 SMBus 설정 복원 확인 완료 config_restore_attempted=1 config_restore_error=0 ec_base=0x%04x saved_cfg1=%d saved_cfg2=%d\n",
			nct6687_ec_base, saved_cfg1, saved_cfg2);
	}
	return ret;
}

static int nct6687_smbus_poll(void)
{
	int timeout = 100;
	int ret;
	u8 status;

	while (timeout-- > 0) {
		usleep_range(400, 600);
		status = nct6687_ec_read(EC_PAGE_SMBUS, EC_REG_STATUS);
		if (!(status & EC_STATUS_BUSY)) {
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_STATUS, EC_CMD_RESET);
			ret = nct6687_smbus_wait_idle();
			if (ret) {
				transport_error = ret;
				pr_err("nct6687_smbus: 전송 완료 RESET 뒤 유휴 확인 실패 transport_error=%d\n", ret);
				return ret;
			}
			return (status & EC_STATUS_SUCCESS) ? 0 : -ENXIO;
		}
	}
	nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_STATUS, EC_CMD_RESET);
	ret = nct6687_smbus_wait_idle();
	if (ret) {
		transport_error = ret;
		pr_err("nct6687_smbus: 시간초과 RESET 뒤 유휴 확인 실패 transport_error=%d\n", ret);
		return ret;
	}
	return -ETIMEDOUT;
}

static s32 nct6687_smbus_xfer(struct i2c_adapter *adapter, u16 addr,
			      unsigned short flags, char read_write,
			      u8 command, int size,
			      union i2c_smbus_data *data)
{
	int ret = 0;

	mutex_lock(&nct6687_smbus_lock);
	if (transport_error) {
		ret = transport_error;
		goto unlock;
	}
	ret = nct6687_smbus_wait_idle();
	if (ret)
		goto unlock;
	if (nct6687_ec_read(EC_PAGE_SMBUS, EC_REG_BUS_CFG1) != 0x00 ||
	    nct6687_ec_read(EC_PAGE_SMBUS, EC_REG_BUS_CFG2) != 0x03) {
		transport_error = -EIO;
		ret = transport_error;
		pr_err("nct6687_smbus: 적용한 CFG가 변경되어 전송 중단 transport_error=%d\n", ret);
		goto unlock;
	}

	switch (size) {
	case I2C_SMBUS_QUICK:
		nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_SLAVE_ADDR, (u8)(addr << 1));
		nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_CMD, 0x00);
		nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_PROTOCOL, EC_PROTO_READ_BYTE);
		nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_STATUS, EC_CMD_START);
		ret = nct6687_smbus_poll();
		break;

	case I2C_SMBUS_BYTE:
		if (read_write == I2C_SMBUS_READ) {
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_SLAVE_ADDR, (u8)(addr << 1));
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_CMD, 0x00);
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_PROTOCOL, EC_PROTO_READ_BYTE);
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_STATUS, EC_CMD_START);
			ret = nct6687_smbus_poll();
			if (!ret)
				data->byte = nct6687_ec_read(EC_PAGE_SMBUS, EC_REG_DATA_OUT0);
		} else {
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_SLAVE_ADDR, (u8)(addr << 1));
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_CMD, command);
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_DATA_IN0, 0x00);
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_PROTOCOL, EC_PROTO_WRITE_BYTE);
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_STATUS, EC_CMD_START);
			ret = nct6687_smbus_poll();
		}
		break;

	case I2C_SMBUS_BYTE_DATA:
		nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_SLAVE_ADDR, (u8)(addr << 1));
		nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_CMD, command);
		if (read_write == I2C_SMBUS_READ) {
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_PROTOCOL, EC_PROTO_READ_BYTE);
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_STATUS, EC_CMD_START);
			ret = nct6687_smbus_poll();
			if (!ret)
				data->byte = nct6687_ec_read(EC_PAGE_SMBUS, EC_REG_DATA_OUT0);
		} else {
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_DATA_IN0, data->byte);
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_PROTOCOL, EC_PROTO_WRITE_BYTE);
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_STATUS, EC_CMD_START);
			ret = nct6687_smbus_poll();
		}
		break;

	case I2C_SMBUS_WORD_DATA:
		nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_SLAVE_ADDR, (u8)(addr << 1));
		nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_CMD, command);
		if (read_write == I2C_SMBUS_READ) {
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_PROTOCOL, EC_PROTO_READ_WORD);
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_STATUS, EC_CMD_START);
			ret = nct6687_smbus_poll();
			if (!ret) {
				u8 low = nct6687_ec_read(EC_PAGE_SMBUS, EC_REG_DATA_OUT0);
				u8 high = nct6687_ec_read(EC_PAGE_SMBUS, EC_REG_DATA_OUT1);
				data->word = ((u16)high << 8) | low;
			}
		} else {
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_DATA_IN0, (u8)(data->word & 0xFF));
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_DATA_IN1, (u8)((data->word >> 8) & 0xFF));
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_PROTOCOL, EC_PROTO_WRITE_WORD);
			nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_STATUS, EC_CMD_START);
			ret = nct6687_smbus_poll();
		}
		break;

	default:
		ret = -EOPNOTSUPP;
		break;
	}

unlock:
	mutex_unlock(&nct6687_smbus_lock);
	return ret;
}

static u32 nct6687_smbus_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
	       I2C_FUNC_SMBUS_BYTE | I2C_FUNC_SMBUS_QUICK;
}

static const struct i2c_algorithm nct6687_smbus_algorithm = {
	.smbus_xfer    = nct6687_smbus_xfer,
	.functionality = nct6687_smbus_func,
};

/* nct6687의 동일한 정규화 규칙을 사용하되 미지의 정렬은 거부한다. */
static int nct6687_smbus_normalize_base(u16 raw, u16 *normalized)
{
	u16 base = raw;

	if ((base & 0x07) == 0x05)
		base &= 0xFFF8;
	if (base < 0x100 || (base & 0xF007) != 0)
		return -ENODEV;
	*normalized = base;
	return 0;
}

int nct6687_smbus_init(struct i2c_adapter **adapter_out)
{
	struct i2c_adapter *adapter;
	struct resource resource = { .name = "nct6687_smbus", .flags = IORESOURCE_IO };
	u16 base, raw_base, verified_base, chip_id;
	int ret;

	*adapter_out = NULL;

	/* Configure NCT6686 / NCT6687 Logical Device 0x0B (EC Space) */
	if (!request_muxed_region(SIO_REG_PORT, 2, "nct6687_smbus"))
		return -EBUSY;

	outb(0x87, SIO_REG_PORT);
	outb(0x87, SIO_REG_PORT);
	outb(SIO_REG_DEVID, SIO_REG_PORT);
	chip_id = (u16)inb(SIO_DATA_PORT) << 8;
	outb(SIO_REG_DEVID + 1, SIO_REG_PORT);
	chip_id |= inb(SIO_DATA_PORT);
	if ((chip_id & 0xFFF0) != 0xD440 && (chip_id & 0xFFF0) != 0xD590) {
		ret = -ENODEV;
		goto leave_superio;
	}

	outb(SIO_LDN_SEL, SIO_REG_PORT);
	outb(SIO_LDN_EC, SIO_DATA_PORT);

	outb(SIO_REG_ACTIVATE, SIO_REG_PORT);
	if (!(inb(SIO_DATA_PORT) & 0x01)) {
		ret = -ENODEV;
		goto leave_superio;
	}

	outb(SIO_REG_ADDR_HIGH, SIO_REG_PORT);
	raw_base = (u16)inb(SIO_DATA_PORT) << 8;
	outb(SIO_REG_ADDR_LOW, SIO_REG_PORT);
	raw_base |= (u16)inb(SIO_DATA_PORT);

	ret = nct6687_smbus_normalize_base(raw_base, &base);
	if (ret)
		goto leave_superio;
	/* 펌웨어와 다른 호스트의 변경이 끼어들었으면 주소를 추측하지 않는다. */
	outb(SIO_REG_ADDR_HIGH, SIO_REG_PORT);
	verified_base = (u16)inb(SIO_DATA_PORT) << 8;
	outb(SIO_REG_ADDR_LOW, SIO_REG_PORT);
	verified_base |= (u16)inb(SIO_DATA_PORT);
	if (verified_base != raw_base) {
		ret = -EIO;
		goto leave_superio;
	}

	ret = 0;
leave_superio:
	outb(0xAA, SIO_REG_PORT);
	release_region(SIO_REG_PORT, 2);
	if (ret)
		return ret;

	nct6687_ec_base = base;
	ec_base = base;
	resource.start = base;
	resource.end = base + NCT6687_EC_PORT_LEN - 1;
	ret = acpi_check_resource_conflict(&resource);
	if (ret)
		return ret;

	/* Reserve EC Port 0 (base to base+3) */
	if (!request_region(nct6687_ec_base, NCT6687_EC_PORT_LEN, "nct6687_smbus")) {
		pr_err("nct6687_smbus: failed to request EC port 0x%04x\n", nct6687_ec_base);
		return -EBUSY;
	}

	/* 이미 진행 중인 외부 전송은 초기화하지 않고 적재를 거부한다. */
	ret = nct6687_smbus_wait_idle();
	if (ret) {
		release_region(nct6687_ec_base, NCT6687_EC_PORT_LEN);
		return ret;
	}
	saved_cfg1 = nct6687_ec_read(EC_PAGE_SMBUS, EC_REG_BUS_CFG1);
	saved_cfg2 = nct6687_ec_read(EC_PAGE_SMBUS, EC_REG_BUS_CFG2);
	nct6687_config_saved = true;
	pr_info("nct6687_smbus: 최초 설정 저장 ec_base=0x%04x raw_base=0x%04x saved_cfg1=%d saved_cfg2=%d config_restore_attempted=0\n",
		nct6687_ec_base, raw_base, saved_cfg1, saved_cfg2);
	/* Initialize EC Page 4 SMBus master clock & config */
	nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_BUS_CFG1, 0x00);
	nct6687_ec_write(EC_PAGE_SMBUS, EC_REG_BUS_CFG2, 0x03);
	if (nct6687_ec_read(EC_PAGE_SMBUS, EC_REG_BUS_CFG1) != 0x00 ||
	    nct6687_ec_read(EC_PAGE_SMBUS, EC_REG_BUS_CFG2) != 0x03) {
		pr_err("nct6687_smbus: CFG 적용 재읽기 불일치\n");
		ret = -EIO;
		goto restore_config;
	}

	adapter = kzalloc(sizeof(*adapter), GFP_KERNEL);
	if (!adapter) {
		ret = -ENOMEM;
		goto restore_config;
	}

	adapter->owner = THIS_MODULE;
	/* 이미 아는 0x60 장치만 등록하며 다른 드라이버의 자동 검색은 열지 않는다. */
	adapter->class = 0;
	adapter->algo  = &nct6687_smbus_algorithm;
	strscpy(adapter->name, "nct6687 SMBus master 0", sizeof(adapter->name));

	/* Register the I2C adapter with Linux kernel */
	ret = i2c_add_adapter(adapter);
	if (ret) {
		pr_err("nct6687_smbus: failed to add I2C adapter (%d)\n", ret);
		kfree(adapter);
		goto restore_config;
	}

	*adapter_out = adapter;
	pr_info("nct6687_smbus: registered '%s' at I/O 0x%04x\n",
		adapter->name, nct6687_ec_base);
	return 0;

restore_config:
	if (nct6687_smbus_restore_config())
		ret = config_restore_error;
	release_region(nct6687_ec_base, NCT6687_EC_PORT_LEN);
	return ret;
}

void nct6687_smbus_exit(struct i2c_adapter *adapter)
{
	if (adapter) {
		i2c_del_adapter(adapter);
		kfree(adapter);
	}

	mutex_lock(&nct6687_smbus_lock);
	nct6687_smbus_restore_config();
	mutex_unlock(&nct6687_smbus_lock);
	release_region(nct6687_ec_base, NCT6687_EC_PORT_LEN);
}

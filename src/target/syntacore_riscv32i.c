#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "target/target.h"
#include "target/target_type.h"
#include "target/breakpoints.h"
#include "target/register.h"
#include "helper/log.h"
#include "helper/binarybuffer.h"
#include "helper/types.h"
#include "jtag/jtag.h"

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <memory.h>

#define XLEN 32
#define FLEN 32
#define LOCAL_CONCAT(x,y) x##y
// #define STATIC_ASSERT(e) typedef char LOCAL_CONCAT(___my_static_assert,__LINE__)[1 - 2 * !(e)]
#define STATIC_ASSERT(e) {enum {___my_static_assert = 1 / (!!(e)) };}
#define ARRAY_LEN(arr) (sizeof (arr) / sizeof (arr)[0])
#define BIT_NUM_TO_MASK(bit_num) (1 << (bit_num))
#define LOW_BITS_MASK(n) (~(~0 << (n)))
#define NUM_BITS_TO_SIZE(num_bits) ( ( (size_t)(num_bits) + (CHAR_BIT - 1) ) / CHAR_BIT )
#define RV_SCALL() (0x00100073u)

enum TAP_IR
{
	TAP_INSTR_DBG_ID = 3,
	TAP_INSTR_BLD_ID = 4,
	TAP_INSTR_DBG_STATUS = 5,
	TAP_INSTR_DAP_CTRL = 6,
	TAP_INSTR_DAP_CTRL_RD = 7,
	TAP_INSTR_DAP_CMD = 8,
	TAP_INSTR_IDCODE = 0xE,  ///< recommended
	TAP_INSTR_BYPASS = 0xF,  ///< mandatory
};

enum TAP_DR_LEN
{
	TAP_LEN_IDCODE = 32,  ///< mandatory
	TAP_LEN_DBG_ID = 32,
	TAP_LEN_BLD_ID = 32,
	TAP_LEN_DBG_STATUS = 32,
	TAP_LEN_DAP_CTRL_UNIT = 2,
	TAP_LEN_DAP_CTRL_FGROUP = 2,
	TAP_LEN_DAP_CTRL = TAP_LEN_DAP_CTRL_UNIT + TAP_LEN_DAP_CTRL_FGROUP,
	TAP_LEN_DAP_CMD_OPCODE = 4,
	TAP_LEN_DAP_CMD_OPCODE_EXT = 32,
	TAP_LEN_DAP_CMD = TAP_LEN_DAP_CMD_OPCODE + TAP_LEN_DAP_CMD_OPCODE_EXT,
	TAP_LEN_BYPASS = 1,  ///< mandatory
};

enum type_dbgc_unit_id_e
{
	DBGC_UNIT_ID_HART_0 = 0,
	DBGC_UNIT_ID_HART_1 = 1,
	DBGC_UNIT_ID_CORE = 3,
};

enum DBGC_FGRP
{
	DBGC_FGRP_HART_REGTRANS = 0,
	DBGC_FGRP_HART_DBGCMD = 1,
	DBGC_FGRP_CORE_REGTRANS = 0,
};

enum type_dbgc_core_dbg_sts_reg_bits_e
{
	DBGC_CORE_CDSR_HART0_DMODE_BIT = 0,
	DBGC_CORE_CDSR_HART0_RST_BIT = 1,
	DBGC_CORE_CDSR_HART0_ERR_BIT = 3,
	DBGC_CORE_CDSR_LOCK_BIT = 30,
	DBGC_CORE_CDSR_READY_BIT = 31,
};

enum DBGC_DAP_OPCODE_DBGCMD
{
	DBGC_DAP_OPCODE_DBGCMD_DBG_CTRL = 0,
	DBGC_DAP_OPCODE_DBGCMD_CORE_EXEC = 1,
	DBGC_DAP_OPCODE_DBGCMD_DBGDATA_WR = 2,
	DBGC_DAP_OPCODE_DBGCMD_UNLOCK = 3,
};

enum DAP_OPSTATUS_BITS
{
	DAP_OPSTATUS_EXCEPT = 0,
	DAP_OPSTATUS_ERROR = 1,
	DAP_OPSTATUS_LOCK = 2,
	DAP_OPSTATUS_READY = 3,
};

enum
{
	DBGC_HART_REGS_DBG_CTRL = 0,
	DBGC_HART_REGS_DBG_STS = 1,
	DBGC_HART_REGS_DMODE_ENBL = 2,
	DBGC_HART_REGS_DMODE_CAUSE = 3,
	DBGC_HART_REGS_CORE_INSTR = 4,
	DBGC_HART_REGS_DBG_DATA = 5,
};

struct This_Arch
{
	struct reg_cache *m_reg_cache;
	/// @todo reasons
	enum target_debug_reason nc_poll_requested;
	int error_code;
};

typedef struct reg_arch_type reg_arch_type;
typedef struct target_type target_type;
typedef struct scan_field scan_field;
typedef struct target target;
typedef struct This_Arch This_Arch;
typedef struct reg reg;

/// Error code handling
///@{
static int
get_error_code(target const* const restrict restrict p_target)
{
	assert(p_target);
	This_Arch const* const restrict p_arch = p_target->arch_info;
	assert(p_arch);
	return p_arch->error_code;
}

static int
update_error_code(target const* const restrict p_target, int const a_error_code)
{
	assert(p_target);
	This_Arch* const restrict p_arch = p_target->arch_info;
	assert(p_arch);
	if ( ERROR_OK == get_error_code(p_target) && ERROR_OK != a_error_code ) {
		p_arch->error_code = a_error_code;
	}
	return get_error_code(p_target);
}

static int
clear_error_code(target const* const restrict p_target)
{
	assert(p_target);
	This_Arch* const const restrict p_arch = p_target->arch_info;
	assert(p_arch);
	int const result = get_error_code(p_target);
	p_arch->error_code = ERROR_OK;
	return result;
}
///@}

/// TAPs methods
/// @{
static void
IR_select(target const* const restrict p_target, enum TAP_IR const new_instr)
{
	assert(p_target);
#if 0
	if ( buf_get_u32(tap->cur_instr, 0u, tap->ir_length) != new_instr ) {
#endif
		assert(p_target->tap);
		assert(p_target->tap->ir_length == 4);
		uint8_t out_buffer[NUM_BITS_TO_SIZE(4)] = {};
		STATIC_ASSERT(NUM_BITS_TO_SIZE(4) == 1u);
		STATIC_ASSERT(sizeof out_buffer == 1u);
		buf_set_u32(out_buffer, 0, 4, new_instr);
		scan_field field =
		{
			.num_bits = p_target->tap->ir_length,
			.out_value = out_buffer,
		};
		jtag_add_ir_scan(p_target->tap, &field, TAP_IDLE);
		LOG_DEBUG("irscan %s %d", p_target->cmd_name, new_instr);
		// force jtag_execute_queue() because field referenced local variable out_buffer
		if ( update_error_code(p_target, jtag_execute_queue()) != ERROR_OK ) {
			LOG_ERROR("Error %d", get_error_code(p_target));
		}
#if 0
	}
#endif
}

static uint32_t
DBG_STATUS_get(target const* const restrict p_target)
{
	assert(p_target);
	assert(p_target->tap);
	IR_select(p_target, TAP_INSTR_DBG_STATUS);
	if ( get_error_code(p_target) != ERROR_OK ) {
		return 0u;
	}

	uint8_t result_buffer[NUM_BITS_TO_SIZE(TAP_LEN_DBG_STATUS)] = {};
	scan_field field =
	{
		.num_bits = TAP_LEN_DBG_STATUS,
		.in_value = result_buffer,
	};
	jtag_add_dr_scan(p_target->tap, 1, &field, TAP_IDLE);

	// enforce jtag_execute_queue() to obtain result
	if ( update_error_code(p_target, jtag_execute_queue()) != ERROR_OK ) {
		LOG_ERROR("JTAG error %d", get_error_code(p_target));
		return 0;
	}

	STATIC_ASSERT(NUM_BITS_TO_SIZE(TAP_LEN_DBG_STATUS) <= sizeof(uint32_t));
	uint32_t const result = buf_get_u32(result_buffer, 0, 32);
	LOG_DEBUG("drscan %s %d 0 --> %#0x", p_target->cmd_name, field.num_bits, result);

	if ( (result & (BIT_NUM_TO_MASK(DBGC_CORE_CDSR_READY_BIT) | BIT_NUM_TO_MASK(DBGC_CORE_CDSR_LOCK_BIT))) != (uint32_t)BIT_NUM_TO_MASK(DBGC_CORE_CDSR_READY_BIT) ) {
		LOG_WARNING("TAP_INSTR_DBG_STATUS is %x!", result);
		update_error_code(p_target, ERROR_TARGET_FAILURE);
	}
	return result;
}

static void
DAP_CTRL_REG_set(target const* const restrict p_target, enum type_dbgc_unit_id_e const dap_unit, enum DBGC_FGRP const dap_group)
{
	assert(p_target);
	assert(
		(
		(
		(dap_unit == DBGC_UNIT_ID_HART_0 && 0 == p_target->coreid) ||
		(dap_unit == DBGC_UNIT_ID_HART_1 && 1 == p_target->coreid)
		) && (dap_group == DBGC_FGRP_HART_REGTRANS || dap_group == DBGC_FGRP_HART_DBGCMD)
		) ||
		(dap_unit == DBGC_UNIT_ID_CORE && dap_group == DBGC_FGRP_HART_REGTRANS)
		);

	uint8_t const set_dap_unit_group = ((((uint8_t)dap_unit & LOW_BITS_MASK(TAP_LEN_DAP_CTRL_UNIT)) << TAP_LEN_DAP_CTRL_FGROUP) | (((uint8_t)dap_group) & LOW_BITS_MASK(TAP_LEN_DAP_CTRL_FGROUP))) & LOW_BITS_MASK(TAP_LEN_DAP_CTRL);
	STATIC_ASSERT(NUM_BITS_TO_SIZE(TAP_LEN_DAP_CTRL) == sizeof set_dap_unit_group);
	{
		/// set unit/group
		IR_select(p_target, TAP_INSTR_DAP_CTRL);
		if ( get_error_code(p_target) != ERROR_OK ) {
			return;
		}
		// clear status bits
		uint8_t status = 0;
		STATIC_ASSERT(NUM_BITS_TO_SIZE(TAP_LEN_DAP_CTRL) == sizeof status);
		scan_field const field =
		{
			.num_bits = TAP_LEN_DAP_CTRL,
			.out_value = &set_dap_unit_group,
			.in_value = &status,
		};
		jtag_add_dr_scan(p_target->tap, 1, &field, TAP_IDLE);
		// enforce jtag_execute_queue() to get status
		if ( update_error_code(p_target, jtag_execute_queue()) != ERROR_OK ) {
			LOG_ERROR("JTAG error %d", get_error_code(p_target));
			return;
		}
		LOG_DEBUG("drscan %s %d %#0x --> %#0x", p_target->cmd_name, field.num_bits, set_dap_unit_group, status);
		if ( (status & BIT_NUM_TO_MASK(DAP_OPSTATUS_READY)) != BIT_NUM_TO_MASK(DAP_OPSTATUS_READY) ) {
			LOG_ERROR("TAP status %0x", (uint32_t)status);
			update_error_code(p_target, ERROR_TARGET_FAILURE);
			return;
		}
	}

	{
		/// verify unit/group
		IR_select(p_target, TAP_INSTR_DAP_CTRL_RD);
		if ( get_error_code(p_target) != ERROR_OK ) {
			return;
		}
		uint8_t get_dap_unit_group = 0;
		STATIC_ASSERT(NUM_BITS_TO_SIZE(TAP_LEN_DAP_CTRL) == sizeof get_dap_unit_group);
		scan_field const field =
		{
			.num_bits = TAP_LEN_DAP_CTRL,
			.in_value = &get_dap_unit_group,
		};
		// enforce jtag_execute_queue() to get get_dap_unit_group
		jtag_add_dr_scan(p_target->tap, 1, &field, TAP_IDLE);
		if ( update_error_code(p_target, jtag_execute_queue()) != ERROR_OK ) {
			LOG_ERROR("JTAG error %d", get_error_code(p_target));
			return;
		}
		LOG_DEBUG("drscan %s %d %#0x --> %#0x", p_target->cmd_name, field.num_bits, 0, get_dap_unit_group);
		if ( get_dap_unit_group != set_dap_unit_group ) {
			LOG_ERROR("Unit/Group verification error: set %#0x, but get %#0x!", set_dap_unit_group, get_dap_unit_group);
			update_error_code(p_target, ERROR_TARGET_FAILURE);
			return;
		}
	}
}

static int
HART_DBG_STATUS_get(target const* const restrict p_target)
{
	assert(p_target);
	/// Only 1 HART available
	assert(p_target->coreid == 0);
	uint8_t const status = (DBG_STATUS_get(p_target) >> p_target->coreid) & 0xFFu;
	if ( get_error_code(p_target) != ERROR_OK ) {
		return TARGET_UNKNOWN;
	}
	LOG_DEBUG("status is %x", (uint32_t)status);
	if ( status & BIT_NUM_TO_MASK(DBGC_CORE_CDSR_HART0_ERR_BIT) ) {
		LOG_DEBUG("HART_DBG_STATUS == %s", "TARGET_UNKNOWN");
		return TARGET_UNKNOWN;
	} else if ( status & BIT_NUM_TO_MASK(DBGC_CORE_CDSR_HART0_RST_BIT) ) {
		LOG_DEBUG("HART_DBG_STATUS == %s", "TARGET_RESET");
		return TARGET_RESET;
	} else if ( status & BIT_NUM_TO_MASK(DBGC_CORE_CDSR_HART0_DMODE_BIT) ) {
		LOG_DEBUG("HART_DBG_STATUS == %s", "TARGET_HALTED");
		return TARGET_HALTED;
	} else {
		LOG_DEBUG("HART_DBG_STATUS == %s", "TARGET_RUNNING");
		return TARGET_RUNNING;
	}
}

static uint32_t
DAP_CMD_scan(target const* const restrict p_target, uint8_t const DAP_OPCODE, uint32_t const DAP_OPCODE_EXT)
{
	IR_select(p_target, TAP_INSTR_DAP_CMD);
	if ( get_error_code(p_target) != ERROR_OK ) {
		return 0;
	}
	uint32_t const dap_opcode_ext = DAP_OPCODE_EXT;
	uint8_t const dap_opcode = DAP_OPCODE;
	uint8_t DAP_OPSTATUS = 0;
	uint32_t DBG_DATA = 0;
	scan_field const fields[2] =
	{
		[0] = {
			.num_bits = TAP_LEN_DAP_CMD_OPCODE_EXT,
			.out_value = (uint8_t const*)&dap_opcode_ext,
			.in_value = (uint8_t*)&DBG_DATA,
		},
		[1] =
			{
				.num_bits = TAP_LEN_DAP_CMD_OPCODE,
				.out_value = &dap_opcode,
				.in_value = &DAP_OPSTATUS,
			},
	};
	STATIC_ASSERT(NUM_BITS_TO_SIZE(TAP_LEN_DAP_CMD_OPCODE_EXT) == sizeof dap_opcode_ext);
	STATIC_ASSERT(NUM_BITS_TO_SIZE(TAP_LEN_DAP_CMD_OPCODE_EXT) == sizeof DBG_DATA);
	STATIC_ASSERT(NUM_BITS_TO_SIZE(TAP_LEN_DAP_CMD_OPCODE) == sizeof dap_opcode);
	STATIC_ASSERT(NUM_BITS_TO_SIZE(TAP_LEN_DAP_CMD_OPCODE) == sizeof DAP_OPSTATUS);

	assert(p_target->tap);
	jtag_add_dr_scan(p_target->tap, ARRAY_LEN(fields), fields, TAP_IDLE);
	// enforse jtag_execute_queue() to get values
	if ( update_error_code(p_target, jtag_execute_queue()) != ERROR_OK ) {
		LOG_ERROR("JTAG error %d", get_error_code(p_target));
		return DBG_DATA;
	}

	LOG_DEBUG("drscan %s %d %#0x %d %#0x --> %#0x %#0x", p_target->cmd_name, fields[0].num_bits, dap_opcode_ext, fields[1].num_bits, dap_opcode, DBG_DATA, DAP_OPSTATUS);

	if ( (DAP_OPSTATUS & BIT_NUM_TO_MASK(DAP_OPSTATUS_READY)) != BIT_NUM_TO_MASK(DAP_OPSTATUS_READY) ) {
		LOG_ERROR("DAP_OPSTATUS == %#0x", (uint32_t)DAP_OPSTATUS);
		update_error_code(p_target, ERROR_TARGET_FAILURE);
	}
	return DBG_DATA;
}

/// @}

static void
reg_x_operation_conditions_check(reg *p_reg)
{
	assert(p_reg);
	target* p_target = p_reg->arch_info;
	assert(p_target);
	if ( p_reg->number > 32 ) {
		LOG_WARNING("Bad reg id =%d for register %s", p_reg->number, p_reg->name);
		update_error_code(p_target, ERROR_TARGET_RESOURCE_NOT_AVAILABLE);
		return ;
	}
	assert(p_target);
	if ( p_target->state != TARGET_HALTED ) {
		LOG_ERROR("Target not halted");
		update_error_code(p_target, ERROR_TARGET_NOT_HALTED);
	}
}

static int
reg_x_get(reg *p_reg)
{
	reg_x_operation_conditions_check(p_reg);
	target* p_target = p_reg->arch_info;
	assert(p_target);
	if ( get_error_code(p_target) ) {
		return clear_error_code(p_target);
	}

	/// @todo implement HW register read
#if 0
	uint32_t const value = read_core_register(p_reg->number);
	buf_set_u32(p_reg->value, 0, 32, value);
	p_reg->valid = true;
#endif
	p_reg->dirty = true;

	return ERROR_OK;
}

static int
reg_x_set(reg *p_reg, uint8_t *buf)
{
	reg_x_operation_conditions_check(p_reg);
	target* p_target = p_reg->arch_info;
	assert(p_target);
	if ( get_error_code(p_target) ) {
		return clear_error_code(p_target);
	}

	uint32_t const value = buf_get_u32(buf, 0, 32);
	LOG_DEBUG("Updating cache for register %s <-- %08x", p_reg->name, value);

	buf_set_u32(p_reg->value, 0, 32, value);
	p_reg->valid = true;
	p_reg->dirty = true;

	return ERROR_OK;
}

static reg_arch_type const reg_x_accessors =
{
	.get = reg_x_get,
	.set = reg_x_set,
};

static int
reg_f_get(reg *p_reg)
{
	LOG_WARNING("NOT IMPLEMENTED");
	return ERROR_OK;
}

static int
reg_f_set(reg *p_reg, uint8_t *buf)
{
	LOG_ERROR("NOT IMPLEMENTED");
	return ERROR_OK;
}

static reg_arch_type const reg_f_accessors =
{
	.get = reg_f_get,
	.set = reg_f_set,
};


static reg const reg_def_array[] = {
	// Hard-wired zero
	{.name = "x0",  .number = 0, .caller_save = false, .dirty = false, .valid = true,  .exist = true, .size = XLEN, .type = NULL},

	// Return address
	{.name = "x1", .number = 1, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},

	// Stack pointer
	{.name = "x2", .number = 2, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},

	// Global pointer
	{.name = "x3", .number = 3, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},

	// Thread pointer
	{.name = "x4", .number = 4, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},

	// Temporaries
	{.name = "x5", .number = 5, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x6", .number = 6, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x7", .number = 7, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},

	// Saved register/frame pointer
	{.name = "x8", .number = 8, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},

	// Saved register
	{.name = "x9", .number = 9, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},

	// Function arguments/return values
	{.name = "x10", .number = 10, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x11", .number = 11, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},

	// Function arguments
	{.name = "x12", .number = 12, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x13", .number = 13, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x14", .number = 14, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x15", .number = 15, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x16", .number = 16, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x17", .number = 17, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},

	// Saved registers
	{.name = "x18", .number = 18, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x19", .number = 19, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x20", .number = 20, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x21", .number = 21, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x22", .number = 22, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x23", .number = 23, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x24", .number = 24, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x25", .number = 25, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x26", .number = 26, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x27", .number = 27, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},

	// Temporaries
	{.name = "x28", .number = 28, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x29", .number = 29, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x30", .number = 30, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},
	{.name = "x31", .number = 31, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = &reg_x_accessors},

	{.name = "pc",  .number = 50, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = XLEN, .type = NULL},

	// FP temporaries
	{.name = "f0",  .number = 100, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},
	{.name = "f1",  .number = 101, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},
	{.name = "f2",  .number = 102, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},
	{.name = "f3",  .number = 103, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},
	{.name = "f4",  .number = 104, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},
	{.name = "f5",  .number = 105, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},
	{.name = "f6",  .number = 106, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},
	{.name = "f7",  .number = 107, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},

	// FP saved registers
	{.name = "f8",  .number = 108, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},
	{.name = "f9",  .number = 109, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},

	// FP arguments/return values
	{.name = "f10", .number = 110, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},
	{.name = "f11", .number = 111, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},

	// FP arguments
	{.name = "f12", .number = 112, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},
	{.name = "f13", .number = 113, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},
	{.name = "f14", .number = 114, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},
	{.name = "f15", .number = 115, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},
	{.name = "f16", .number = 116, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},
	{.name = "f17", .number = 117, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_f_accessors},

	// FP saved registers
	{.name = "f18", .number = 118, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_x_accessors},
	{.name = "f19", .number = 119, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_x_accessors},
	{.name = "f20", .number = 120, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_x_accessors},
	{.name = "f21", .number = 121, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_x_accessors},
	{.name = "f22", .number = 122, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_x_accessors},
	{.name = "f23", .number = 123, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_x_accessors},
	{.name = "f24", .number = 124, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_x_accessors},
	{.name = "f25", .number = 125, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_x_accessors},
	{.name = "f26", .number = 126, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_x_accessors},
	{.name = "f27", .number = 127, .caller_save = false, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_x_accessors},

	// FP temporaries
	{.name = "f28", .number = 128, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_x_accessors},
	{.name = "f29", .number = 129, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_x_accessors},
	{.name = "f30", .number = 130, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_x_accessors},
	{.name = "f31", .number = 131, .caller_save = true, .dirty = false, .valid = false, .exist = true, .size = FLEN, .type = &reg_x_accessors},
};

static struct reg_cache*
reg_cache__create(target * p_target)
{
	typedef struct reg_cache reg_cache;
	reg_cache* const p_obj = calloc(1, sizeof(struct reg_cache));
	assert(p_obj);

	static size_t const num_regs = ARRAY_LEN(reg_def_array);
	reg* const reg_list = calloc(num_regs, sizeof(reg));
	reg* p_dest_reg = reg_list;
	reg const* p_src_reg = reg_def_array;
	for ( size_t i = 0; i < num_regs; ++i ) {
		*p_dest_reg = *p_src_reg;
		p_dest_reg->value = malloc(NUM_BITS_TO_SIZE(p_src_reg->size));
		p_dest_reg->arch_info = p_target;
		++p_src_reg;
		++p_dest_reg;
	}
	reg_cache const the_reg_cache = {
		.name = "syntacore_riscv32 registers",
		.reg_list = reg_list,
		.num_regs = num_regs,
	};
	*p_obj = the_reg_cache;
	return p_obj;
}

static int
this_target_create(target *p_target, struct Jim_Interp *interp)
{
	assert(p_target);

	This_Arch the_arch = {
		.nc_poll_requested = DBG_REASON_DBGRQ,
		.m_reg_cache = reg_cache__create(p_target)
	};
	This_Arch* p_arch_info = calloc(1, sizeof(This_Arch));
	*p_arch_info = the_arch;

	p_target->arch_info = p_arch_info;
	return ERROR_OK;
}

static void
save_context(target *p_target)
{
	LOG_ERROR("Unimplemented");
}

static void
restore_context(target *p_target)
{
	LOG_ERROR("Unimplemented");
}

static int
this_poll(target *p_target)
{
	p_target->state = HART_DBG_STATUS_get(p_target);
	if ( p_target->state == TARGET_RUNNING ) {
		return clear_error_code(p_target);
	}

	This_Arch* p_arch = p_target->arch_info;
	if ( p_arch->nc_poll_requested == DBG_REASON_DBGRQ ) {
		p_target->debug_reason = DBG_REASON_DBGRQ;
		return clear_error_code(p_target);
	}

	if ( p_arch->nc_poll_requested == DBG_REASON_SINGLESTEP ) {
		/// @todo enable interrupts
#if 0
		debug_write_register(p_target, DEBUG_CONTROL_COMMAND, 0x0);
#endif
	}
	p_target->debug_reason = p_arch->nc_poll_requested;
	target_call_event_callbacks(p_target, TARGET_EVENT_HALTED);
	p_arch->nc_poll_requested = DBG_REASON_DBGRQ;
	save_context(p_target);  // update reg cache
	return clear_error_code(p_target);
}

static int
this_arch_state(target *p_target)
{
	LOG_ERROR("Unimplemented");
	return clear_error_code(p_target);
}

static int
this_init(struct command_context *cmd_ctx, target *p_target)
{
	return clear_error_code(p_target);
}

static int
this_halt(target *p_target)
{
	assert(p_target);
	{
		// May be already halted?
		{
			// Update state
			int const state = HART_DBG_STATUS_get(p_target);
			if ( get_error_code(p_target) != ERROR_OK ) {
				return clear_error_code(p_target);
			}

			p_target->state = state;
		}

		if ( p_target->state == TARGET_HALTED ) {
			LOG_WARNING("Halt request when RV is already in halted state");
			return clear_error_code(p_target);
		}
	}

	{
		// Try to halt
		DAP_CTRL_REG_set(p_target, p_target->coreid == 0 ? DBGC_UNIT_ID_HART_0 : DBGC_UNIT_ID_HART_1, DBGC_FGRP_HART_DBGCMD);
		if ( get_error_code(p_target) != ERROR_OK ) {
			return clear_error_code(p_target);
		}

		(void)DAP_CMD_scan(p_target, DBGC_DAP_OPCODE_DBGCMD_DBG_CTRL, 1u);
		if ( get_error_code(p_target) != ERROR_OK ) {
			return clear_error_code(p_target);
		}
	}

	{
		// update state
		int state = HART_DBG_STATUS_get(p_target);
		if ( get_error_code(p_target) != ERROR_OK ) {
			return clear_error_code(p_target);
		}
		p_target->state = state;
	}

	// Verify that in debug mode
	if ( p_target->state != TARGET_HALTED ) {
		// issue error if we are still running
		LOG_ERROR("RV is not halted after Halt command");
		update_error_code(p_target, ERROR_TARGET_NOT_HALTED);
		return clear_error_code(p_target);
	}

	// OK, halted
	This_Arch* p_arch = p_target->arch_info;
	if ( p_arch->nc_poll_requested ) {
		p_arch->nc_poll_requested = DBG_REASON_DBGRQ;
	}
	p_target->debug_reason = DBG_REASON_DBGRQ;
	return clear_error_code(p_target);
}

static int
this_resume(target *p_target, int current, uint32_t address, int handle_breakpoints, int debug_execution)
{
	// upload reg values into HW
	restore_context(p_target);
	/// @todo issue resume command
	This_Arch* p_arch = p_target->arch_info;
	p_arch->nc_poll_requested = DBG_REASON_BREAKPOINT;
	p_target->state = TARGET_RUNNING;
	return clear_error_code(p_target);
}

static int
this_step(target *p_target, int current, uint32_t address, int handle_breakpoints)
{
	// upload reg values into HW
	restore_context(p_target);
#if 0
	debug_write_register(target, DEBUG_CONTROL_STATUS, DEBUG_CONTROL_STATUS_IRQ_DISABLE);
	debug_write_register(target, DEBUG_CONTROL_COMMAND, DEBUG_CONTROL_COMMAND_STEP);
#endif
	This_Arch* p_arch = (This_Arch*)p_target->arch_info;
	p_arch->nc_poll_requested = DBG_REASON_SINGLESTEP;
	p_target->state = TARGET_RUNNING;
	return clear_error_code(p_target);
}

static inline uint8_t
REGTRANS_scan_type(bool write, uint8_t index)
{
	assert((index & !LOW_BITS_MASK(3)) == 0);
	return (write ? BIT_NUM_TO_MASK(3) : 0) | index;
}

enum
{
	DBGC_CORE_REGS_DEBUG_ID = 0,
	DBGC_CORE_REGS_DBG_CTRL = 1,
	DBGC_CORE_REGS_DBG_STS = 2,
	DBGC_CORE_REGS_DBG_CMD = 3,
};

static int
set_reset_state(target *p_target, bool active)
{
	assert(p_target);
	DAP_CTRL_REG_set(p_target, DBGC_UNIT_ID_CORE, DBGC_FGRP_CORE_REGTRANS);
	if ( get_error_code(p_target) != ERROR_OK ) {
		return clear_error_code(p_target);
	}

	uint32_t const get_old_value1 = DAP_CMD_scan(p_target, REGTRANS_scan_type(false, DBGC_CORE_REGS_DBG_CTRL), 0);
	if ( get_error_code(p_target) != ERROR_OK ) {
		return clear_error_code(p_target);
	}

	static uint32_t const bit_mask = BIT_NUM_TO_MASK(0);

	uint32_t const set_value = (get_old_value1 & ~bit_mask) | (active ? bit_mask : 0u);
	uint32_t const get_old_value2 = DAP_CMD_scan(p_target, REGTRANS_scan_type(true, DBGC_CORE_REGS_DBG_CTRL), set_value);
	if ( get_error_code(p_target) != ERROR_OK ) {
		return clear_error_code(p_target);
	}
	assert(get_old_value2 == get_old_value1);

	{
		// double check
		DAP_CMD_scan(p_target, REGTRANS_scan_type(0, DBGC_CORE_REGS_DBG_CTRL), 0);
		if ( get_error_code(p_target) != ERROR_OK ) {
			return clear_error_code(p_target);
		}
		uint32_t const get_new_value2 = DAP_CMD_scan(p_target, REGTRANS_scan_type(0, DBGC_CORE_REGS_DBG_CTRL), 0);
		if ( get_error_code(p_target) != ERROR_OK ) {
			return clear_error_code(p_target);
		}
		if ( (get_new_value2 & bit_mask) != (set_value & bit_mask) ) {
			LOG_ERROR("Fail to verify reset state: set %#0x, but get %#0x", set_value, get_new_value2);
			update_error_code(p_target, ERROR_TARGET_FAILURE);
			return clear_error_code(p_target);
		}
	}
	p_target->state = HART_DBG_STATUS_get(p_target);
	if ( active && p_target->state != TARGET_RESET ) {
		/// issue error if we are still running
		LOG_ERROR("RV is not resetting after reset assert");
		update_error_code(p_target, ERROR_TARGET_FAILURE);
	} else if ( !active && p_target->state == TARGET_RESET ) {
		LOG_ERROR("RV is stiil in reset after reset deassert");
		update_error_code(p_target, ERROR_TARGET_FAILURE);
	}
	return clear_error_code(p_target);
}

static int
this_assert_reset(target *p_target)
{
	assert(p_target);
	return set_reset_state(p_target, true);
}

static int
this_deassert_reset(target *p_target)
{
	assert(p_target);
	return set_reset_state(p_target, false);
}

static int
this_soft_reset_halt(target *p_target)
{
#if 0
	int retval;
	// assert reset
	if ( (retval = debug_write_register(target, DEBUG_CONTROL_PWR_RST, DEBUG_CONTROL_PWR_RST_HRESET)) != ERROR_OK ) {
		return retval;
	}
	// ...and deassert reset
	return debug_write_register(target, DEBUG_CONTROL_PWR_RST, 0);
#endif
	return clear_error_code(p_target);
}

/// gdb_server expects valid reg values and will use set method for updating reg values
static int
read_mem_word(target * p_target, uint32_t const address, uint32_t* const data)
{
#if 0
	int retval = 0;
	if ( (retval = debug_write_register(target, DEBUG_MEMORY_ACCESS_ADDRESS, address)) != ERROR_OK ) {
		return retval;
	}
	if ( (retval = debug_write_register(target, DEBUG_MEMORY_ACCESS_CMD, DEBUG_MEMORY_ACCESS_CMD_START)) != ERROR_OK ) {
		return retval;
	}
	if ( (retval = debug_read_register(target, DEBUG_MEMORY_ACCESS_RD_DATA, data)) != ERROR_OK ) {
		return retval;
	}
#endif
	LOG_DEBUG("MR A %08X D %08X", address, *data);
	return clear_error_code(p_target);
}

static int
this_read_memory(target *p_target, uint32_t address, uint32_t size, uint32_t count, uint8_t *buffer)
{
	LOG_DEBUG("read_memory at %08X, %d bytes", address, size * count);
	unsigned i = 0;  // byte count
	uint32_t x = 0;  // buffer
	int retval = 0;
	unsigned const buffer_size = count * size;
	// Address is not aligned
	if ( address & 0x3 ) {
		if ( (retval = read_mem_word(p_target, address & (~0x3), &x)) != ERROR_OK ) {
			return clear_error_code(p_target);
		}
		while ( (address + i) & 0x3 && i != buffer_size ) {
			*(buffer + i) = ((uint8_t*)(&x))[(address + i) & 0x3];
			++i;
		}
	}
	for ( ; i + 4 <= buffer_size; i += 4 ) {
		if ( (retval = read_mem_word(p_target, address + i, (uint32_t*)(buffer + i))) != ERROR_OK ) {
			return clear_error_code(p_target);
		}
	}
	if ( buffer_size == i ) {
		return clear_error_code(p_target);
	}
	if ( (retval = read_mem_word(p_target, address + i, &x)) != ERROR_OK ) {
		return clear_error_code(p_target);
	}
	unsigned const word_start_offset = i;
	for ( ; i != buffer_size; ++i ) {
		*(buffer + i) = ((uint8_t*)(&x))[(i - word_start_offset) & 0x3];
	}
	return clear_error_code(p_target);
}

static int
this_examine(target *p_target)
{
	// initialize register values
#if 0
	debug_write_register(target, DEBUG_CONTROL_PWR_RST, DEBUG_CONTROL_PWR_RST_HRESET);
	debug_write_register(target, DEBUG_CONTROL_BREAK, DEBUG_CONTROL_RSTOFBE);
	debug_write_register(target, DEBUG_CONTROL_HALT, DEBUG_CONTROL_HALT_INT3);
	debug_write_register(target, DEBUG_CONTROL_PWR_RST, 0);
#endif
	p_target->state = TARGET_HALTED;
	save_context(p_target);
	target_set_examined(p_target);
	return clear_error_code(p_target);
}

static int
this_add_breakpoint(target *p_target, struct breakpoint *breakpoint)
{
	int retval = 0;
	if ( breakpoint->length != 4 || breakpoint->address & 0x3u || breakpoint->type == BKPT_HARD ) {
		return clear_error_code(p_target);
	}
	if ( (retval = target_read_buffer(p_target, breakpoint->address, breakpoint->length, breakpoint->orig_instr)) != ERROR_OK ) {
		return clear_error_code(p_target);
	}
	if ( (retval = target_write_u32(p_target, breakpoint->address, RV_SCALL())) != ERROR_OK ) {
		return clear_error_code(p_target);
	}
	breakpoint->set = 1;

	return clear_error_code(p_target);
}

static int
this_remove_breakpoint(target *p_target, struct breakpoint *breakpoint)
{
	if ( breakpoint->length != 4 || breakpoint->type == BKPT_HARD ) {
		return clear_error_code(p_target);
	}
	return target_write_buffer(p_target, breakpoint->address, breakpoint->length, breakpoint->orig_instr);
}

static int
this_write_memory(target *p_target, uint32_t address, uint32_t size, uint32_t count, const uint8_t *buffer)
{
	LOG_DEBUG("write_memory at %08X, %d bytes", address, size * count);
#if 0
	unsigned i = 0;  // byte count
	uint32_t x = 0;  // buffer
	int retval = 0;
	unsigned word_start_offset = 0;
	unsigned const buffer_size = count * size;
	// Address is not aligned
	if ( address & 0x3 ) {
		if ( (retval = read_mem_word(target, address & (~0x3), &x)) != ERROR_OK ) {
			return retval;
		}
		while ( (address + i) & 0x3 && i != buffer_size ) {
			((uint8_t*)(&x))[(address + i) & 0x3] = *(buffer + i);
			++i;
		}
		if ( (retval = write_mem_word(target, address & (~0x3), x)) != ERROR_OK ) {
			return retval;
		}
	}
	uint32_t const chunk_write_size = (buffer_size - i) & ~0x3u;
	if ( chunk_write_size < 8 ) {
		for ( ; i + 4 <= buffer_size; i += 4 ) {
			if ( (retval = write_mem_word(target, address + i, *(uint32_t*)(buffer + i))) != ERROR_OK ) {
				return retval;
			}
		}
	} else {
		write_mem_chunk(target, address + i, (uint32_t*)(buffer + i), chunk_write_size / 4);
		i += chunk_write_size;
	}

	if ( buffer_size == i ) {
		return ERROR_OK;
	}
	if ( (retval = read_mem_word(target, address + i, &x)) != ERROR_OK ) {
		return retval;
	}
	word_start_offset = i;
	for ( ; i != buffer_size; ++i ) {
		((uint8_t*)&x)[(i - word_start_offset) & 0x3] = buffer[i];
	}

	return write_mem_word(target, (address + i) & ~0x3, x);
#endif
	return ERROR_OK;
}

static int
this_get_gdb_reg_list(target *p_target, reg **reg_list[], int *reg_list_size, enum target_register_class reg_class)
{
	This_Arch *arch_info = (This_Arch *)p_target->arch_info;

	size_t const num_regs = 33 + (reg_class == REG_CLASS_ALL ? 32 : 0);
	reg** p_reg_array = calloc(num_regs, sizeof(reg*));
	reg *a_reg_list = arch_info->m_reg_cache->reg_list;
	for ( size_t i = 0; i < num_regs; ++i ) {
		p_reg_array[i] = &a_reg_list[i];
	}
	*reg_list_size = num_regs;
	*reg_list = p_reg_array;
	return clear_error_code(p_target);
}

target_type syntacore_riscv32i_target =
{
	.name = "syntacore_riscv32i",

	.poll = this_poll,
	.arch_state = this_arch_state,

	.halt = this_halt,
	.resume = this_resume,
	.step = this_step,

	.assert_reset = this_assert_reset,
	.deassert_reset = this_deassert_reset,
	.soft_reset_halt = this_soft_reset_halt,

	.get_gdb_reg_list = this_get_gdb_reg_list,

	.read_memory = this_read_memory,
	.write_memory = this_write_memory,

	.add_breakpoint = this_add_breakpoint,
	.remove_breakpoint = this_remove_breakpoint,

	.target_create = this_target_create,
	.examine = this_examine,

	.init_target = this_init,
};


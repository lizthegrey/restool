/*
 * Copyright (C) 2014 Freescale Semiconductor, Inc.
 * Author: German Rivera <German.Rivera@freescale.com>
 *	   Lijun Pan <Lijun.Pan@freescale.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation  and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of any
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include "resman.h"
#include "utils.h"
#include "fsl_dpio.h"

/**
 * dpio info command options
 */
enum dpio_info_options {
	INFO_OPT_HELP = 0,
	INFO_OPT_VERBOSE,
};

static struct option dpio_info_options[] = {
	[INFO_OPT_HELP] = {
		.name = "help",
		.has_arg = 0,
		.flag = NULL,
		.val = 0,
	},

	[INFO_OPT_VERBOSE] = {
		.name = "verbose",
		.has_arg = 0,
		.flag = NULL,
		.val = 0,
	},

	{ 0 },
};

C_ASSERT(ARRAY_SIZE(dpio_info_options) <= MAX_NUM_CMD_LINE_OPTIONS + 1);

/**
 * dpio create command options
 */
enum dpio_create_options {
	CREATE_OPT_HELP = 0,
	CREATE_OPT_CHANNEL_MODE,
	CREATE_OPT_NUM_PRIORITIES,
};

static struct option dpio_create_options[] = {
	[CREATE_OPT_HELP] = {
		.name = "help",
		.has_arg = 0,
		.flag = NULL,
		.val = 0,
	},

	[CREATE_OPT_CHANNEL_MODE] = {
		.name = "channel-mode",
		.has_arg = 1,
		.flag = NULL,
		.val = 0,
	},

	[CREATE_OPT_NUM_PRIORITIES] = {
		.name = "num-priorities",
		.has_arg = 1,
		.flag = NULL,
		.val = 0,
	},

	{ 0 },
};

C_ASSERT(ARRAY_SIZE(dpio_create_options) <= MAX_NUM_CMD_LINE_OPTIONS + 1);

/**
 * dpio destroy command options
 */
enum dpio_destroy_options {
	DESTROY_OPT_HELP = 0,
};

static struct option dpio_destroy_options[] = {
	[DESTROY_OPT_HELP] = {
		.name = "help",
		.has_arg = 0,
		.flag = NULL,
		.val = 0,
	},

	{ 0 },
};

C_ASSERT(ARRAY_SIZE(dpio_destroy_options) <= MAX_NUM_CMD_LINE_OPTIONS + 1);

static int cmd_dpio_help(void)
{
	static const char help_msg[] =
		"\n"
		"resman dpio <command> [--help] [ARGS...]\n"
		"Where <command> can be:\n"
		"   info - displays detailed information about a DPIO object.\n"
		"   create - creates a new child DPIO under the root DPRC.\n"
		"   destroy - destroys a child DPIO under the root DPRC.\n"
		"\n"
		"For command-specific help, use the --help option of each command.\n"
		"\n";

	printf(help_msg);
	return 0;
}

static int print_dpio_attr(uint32_t dpio_id)
{
	uint16_t dpio_handle;
	int error;
	struct dpio_attr dpio_attr;
	bool dpio_opened = false;

	error = dpio_open(&resman.mc_io, dpio_id, &dpio_handle);
	if (error < 0) {
		ERROR_PRINTF("dpio_open() failed for dpio.%u with error %d\n",
			     dpio_id, error);
		goto out;
	}
	dpio_opened = true;
	if (0 == dpio_handle) {
		ERROR_PRINTF(
			"dpio_open() returned invalid handle (auth 0) for dpio.%u\n",
			dpio_id);
		error = -ENOENT;
		goto out;
	}

	memset(&dpio_attr, 0, sizeof(dpio_attr));
	error = dpio_get_attributes(&resman.mc_io, dpio_handle, &dpio_attr);
	if (error < 0) {
		ERROR_PRINTF("dpio_get_attributes() failed with error: %d\n",
			     error);
		goto out;
	}
	assert(dpio_id == (uint32_t)dpio_attr.id);

	printf("dpio version: %u.%u\n", dpio_attr.version.major,
	       dpio_attr.version.minor);
	printf("dpio id: %d\n", dpio_attr.id);
	printf(
		"physical address of qbman software portal cache-enabled area: %#llx\n",
		(unsigned long long)dpio_attr.qbman_portal_ce_paddr);
	printf(
		"physical address of qbman software portal cache-inhibited area: %#llx\n",
		(unsigned long long)dpio_attr.qbman_portal_ci_paddr);
	printf("qbman software portal id: %#x\n",
	       (unsigned int)dpio_attr.qbman_portal_id);
	printf("dpio channel mode is: ");
	dpio_attr.channel_mode == 0 ? printf("DPIO_NO_CHANNEL\n") :
	dpio_attr.channel_mode == 1 ? printf("DPIO_LOCAL_CHANNEL\n") :
	printf("wrong mode\n");
	printf("number of priorities is: %#x\n",
	       (unsigned int)dpio_attr.num_priorities);

	error = 0;

out:
	if (dpio_opened) {
		int error2;

		error2 = dpio_close(&resman.mc_io, dpio_handle);
		if (error2 < 0) {
			ERROR_PRINTF("dpio_close() failed with error %d\n",
				     error2);
			if (error == 0)
				error = error2;
		}
	}

	return error;
}

static int print_dpio_verbose(void)
{
	printf("not implemented yet\n");
	return -ENOTSUP;
}

static int print_dpio_info(uint32_t dpio_id)
{
	int error;

	error = print_dpio_attr(dpio_id);
	if (error < 0)
		goto out;

	if (resman.cmd_option_mask & ONE_BIT_MASK(INFO_OPT_VERBOSE)) {
		resman.cmd_option_mask &= ~ONE_BIT_MASK(INFO_OPT_VERBOSE);
		error = print_dpio_verbose();
		goto out;
	}

out:
	return error;
}

static int cmd_dpio_info(void)
{
	static const char usage_msg[] =
	"\n"
	"Usage: resman dpio info <dpio-object> [--verbose]\n"
	"   e.g. resman dpio info dpio.5\n"
	"\n"
	"--verbose\n"
	"   Shows extended/verbose information about the object\n"
	"   e.g. resman dpio info dpio.5 --verbose\n"
	"\n";

	uint32_t obj_id;
	int error;

	if (resman.cmd_option_mask & ONE_BIT_MASK(INFO_OPT_HELP)) {
		printf(usage_msg);
		resman.cmd_option_mask &= ~ONE_BIT_MASK(INFO_OPT_HELP);
		error = 0;
		goto out;
	}

	if (resman.obj_name == NULL) {
		ERROR_PRINTF("<object> argument missing\n");
		printf(usage_msg);
		error = -EINVAL;
		goto out;
	}

	error = parse_object_name(resman.obj_name, "dpio", &obj_id);
	if (error < 0)
		goto out;

	error = print_dpio_info(obj_id);
out:
	return error;
}

static int cmd_dpio_create(void)
{
	static const char usage_msg[] =
		"\n"
		"Usage: resman dpio create [OPTIONS]\n"
		"   e.g. create a DPIO object with all default options:\n"
		"	resman dpio create\n"
		"\n"
		"OPTIONS:\n"
		"if options are not specified, create DPIO by default options\n"
		"--channel-mode=<mode>\n"
		"   Where <mode> is one of:\n"
		"	DPIO_LOCAL_CHANNEL\n"
		"	DPIO_NO_CHANNEL\n"
		"   Default value is DPIO_LOCAL_CHANNEL\n"
		"--num-priorities=<number>\n"
		"   Valid values for <number> are 1-8. Default value is 8.\n"
		"\n";

	int error;
	long val;
	char *endptr;
	char *str;
	struct dpio_cfg dpio_cfg;
	uint16_t dpio_handle;

	if (resman.cmd_option_mask & ONE_BIT_MASK(CREATE_OPT_HELP)) {
		printf(usage_msg);
		resman.cmd_option_mask &= ~ONE_BIT_MASK(CREATE_OPT_HELP);
		return 0;
	}

	if (resman.obj_name != NULL) {
		ERROR_PRINTF("Unexpected argument: \'%s\'\n\n",
			     resman.obj_name);
		printf(usage_msg);
		return -EINVAL;
	}

	if (resman.cmd_option_mask & ONE_BIT_MASK(CREATE_OPT_CHANNEL_MODE)) {
		resman.cmd_option_mask &=
			~ONE_BIT_MASK(CREATE_OPT_CHANNEL_MODE);
		if (strcmp(resman.cmd_option_args[CREATE_OPT_CHANNEL_MODE],
		    "DPIO_LOCAL_CHANNEL") == 0) {
			dpio_cfg.channel_mode = 0;
		} else if (
			strcmp(resman.cmd_option_args[CREATE_OPT_CHANNEL_MODE],
			"DPIO_NO_CHANNEL") == 0) {
			dpio_cfg.channel_mode = 1;
		} else {
			ERROR_PRINTF("wrong channel mode\n");
			printf(usage_msg);
			return -EINVAL;
		}
	} else {
		dpio_cfg.channel_mode = DPIO_LOCAL_CHANNEL;
	}

	if (resman.cmd_option_mask & ONE_BIT_MASK(CREATE_OPT_NUM_PRIORITIES)) {
		resman.cmd_option_mask &=
			~ONE_BIT_MASK(CREATE_OPT_NUM_PRIORITIES);
		errno = 0;
		str = resman.cmd_option_args[CREATE_OPT_NUM_PRIORITIES];
		val = strtol(str, &endptr, 0);

		if (STRTOL_ERROR(str, endptr, val, errno) ||
		    (val < 1 || val > 8)) {
			printf(usage_msg);
			return -EINVAL;
		}

		dpio_cfg.num_priorities = val;
	} else {
		dpio_cfg.num_priorities = 8;
	}

	error = dpio_create(&resman.mc_io, &dpio_cfg, &dpio_handle);
	if (error < 0) {
		ERROR_PRINTF("dpio_create() failed with error %d\n", error);
		return error;
	}

	error = dpio_close(&resman.mc_io, dpio_handle);
	if (error < 0) {
		ERROR_PRINTF("dpio_close() failed with error %d\n", error);
		return error;
	}

	return 0;
}

static int cmd_dpio_destroy(void)
{
	static const char usage_msg[] =
		"\n"
		"Usage: resman dpio destroy <dpio-object>\n"
		"   e.g. resman dpio destroy dpio.9\n"
		"\n";

	int error;
	int error2;
	uint32_t dpio_id;
	uint16_t dpio_handle;
	bool dpio_opened = false;

	if (resman.cmd_option_mask & ONE_BIT_MASK(DESTROY_OPT_HELP)) {
		printf(usage_msg);
		resman.cmd_option_mask &= ~ONE_BIT_MASK(DESTROY_OPT_HELP);
		return 0;
	}

	if (resman.obj_name == NULL) {
		ERROR_PRINTF("<object> argument missing\n");
		printf(usage_msg);
		error = -EINVAL;
		goto out;
	}

	error = parse_object_name(resman.obj_name, "dpio", &dpio_id);
	if (error < 0)
		goto out;

	error = dpio_open(&resman.mc_io, dpio_id, &dpio_handle);
	if (error < 0) {
		ERROR_PRINTF("dpio_open() failed for dpio.%u with error %d\n",
			     dpio_id, error);
		goto out;
	}
	dpio_opened = true;
	if (0 == dpio_handle) {
		ERROR_PRINTF(
			"dpio_open() returned invalid handle (auth 0) for dpio.%u\n",
			dpio_id);
		error = -ENOENT;
		goto out;
	}

	error = dpio_destroy(&resman.mc_io, dpio_handle);
	if (error < 0) {
		ERROR_PRINTF("dpio_destroy() failed with error %d\n", error);
		goto out;
	}
	dpio_opened = false;

out:
	if (dpio_opened) {
		error2 = dpio_close(&resman.mc_io, dpio_handle);
		if (error2 < 0) {
			ERROR_PRINTF("dpio_close() failed with error %d\n",
				     error2);
			if (error == 0)
				error = error2;
		}
	}

	return error;
}

struct object_command dpio_commands[] = {
	{ .cmd_name = "help",
	  .options = NULL,
	  .cmd_func = cmd_dpio_help },

	{ .cmd_name = "info",
	  .options = dpio_info_options,
	  .cmd_func = cmd_dpio_info },

	{ .cmd_name = "create",
	  .options = dpio_create_options,
	  .cmd_func = cmd_dpio_create },

	{ .cmd_name = "destroy",
	  .options = dpio_destroy_options,
	  .cmd_func = cmd_dpio_destroy },

	{ .cmd_name = NULL },
};

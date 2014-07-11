/*
 * Freescale Management Complex (MC) resource manager tool
 *
 * Copyright (C) 2014 Freescale Semiconductor, Inc.
 * Author: German Rivera <German.Rivera@freescale.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include "fsl_dprc.h"
#include "fsl_mc_io_wrapper.h"
#include "fsl_mc_ioctl.h"
#include "utils.h"

/**
 * Bit masks for command-line options:
 */
#define OPT_HELP_MASK		    ONE_BIT_MASK(OPT_HELP)
#define OPT_VERSION_MASK	    ONE_BIT_MASK(OPT_VERSION)
#define OPT_RESOURCES_MASK	    ONE_BIT_MASK(OPT_RESOURCES)
#define OPT_CONTAINER_MASK	    ONE_BIT_MASK(OPT_CONTAINER)
#define OPT_SOURCE_CONTAINER_MASK   ONE_BIT_MASK(OPT_SOURCE_CONTAINER)
#define OPT_DEST_CONTAINER_MASK	    ONE_BIT_MASK(OPT_DEST_CONTAINER)

/**
 * Maximum level of nesting of DPRCs
 */
#define MAX_DPRC_NESTING	    16

/**
 * MC object type string max length (without including the null terminator)
 */
#define OBJ_TYPE_MAX_LENGTH	4

/*
 * TODO: Obtain the following constants from the fsl-mc bus driver via an ioctl
 */
#define MC_PORTALS_BASE_PADDR	((phys_addr_t)0x00080C000000ULL)
#define MC_PORTAL_STRIDE	0x10000
#define MC_PORTAL_SIZE		64
#define MAX_MC_PORTALS		512

#define MC_PORTAL_PADDR_TO_PORTAL_ID(_portal_paddr) \
	(((_portal_paddr) - MC_PORTALS_BASE_PADDR) / MC_PORTAL_STRIDE)

/**
 * Command line option indices for getopt_long()
 */
enum cmd_line_options {
	OPT_HELP = 0,
	OPT_VERSION,
	OPT_RESOURCES,
	OPT_CONTAINER,
	OPT_SOURCE_CONTAINER,
	OPT_DEST_CONTAINER,
	/*
	 * NOTE: New entries must be added above this entry.
	 */
	NUM_CMD_LINE_OPTIONS
};

C_ASSERT(NUM_CMD_LINE_OPTIONS <= 32);

/**
 * Global state of the resman tool
 */
struct resman {
	/**
	 * Bit mask of command-line options not consumed yet
	 */
	uint32_t cmd_line_options_mask;

	/**
	 * Array of option arguments for options found in the command line,
	 * that have arguments. One entry per option.
	 */
	const char *cmd_line_option_arg[NUM_CMD_LINE_OPTIONS];

	/**
	 * resman command found in the command line
	 */
	const char *cmd_name;

	/**
	 * Number of arguments specified for the command found in the command
	 * line
	 */
	int num_cmd_args;

	/**
	 * Array of arguments for the resman command found in the command line
	 */
	char *const *cmd_args;

	/**
	 * MC I/O portal
	 */
	struct mc_portal_wrapper mc_portal;

	/**
	 * Id for the root DPRC in the system
	 */
	uint16_t root_dprc_id;

	/**
	 * Handle for the root DPRC in the system
	 */
	uint16_t root_dprc_handle;
};

typedef int resman_cmd_func_t(void);

struct resman_command {
	char *name;
	resman_cmd_func_t *func;
};

static struct resman resman = {
	.cmd_line_options_mask = 0x0,
	.cmd_name = NULL,
	.num_cmd_args = 0,
};

static const char resman_version[] = "0.1";

static struct option getopt_long_options[] = {
	[OPT_HELP] = {
		.name =  "help",
		.has_arg = 0,
		.flag = NULL,
		.val = 'h',
	},

	[OPT_VERSION] = {
		.name = "version",
		.has_arg = 0,
		.flag = NULL,
		.val = 'v',
	},

	[OPT_RESOURCES] = {
		.name = "resources",
		.has_arg = 0,
		.flag = NULL,
		.val = 'r',
	},

	[OPT_CONTAINER] = {
		.name = "container",
		.has_arg = 1,
		.flag = NULL,
		.val = 'c',
	},

	[OPT_SOURCE_CONTAINER] = {
		.name = "source",
		.has_arg = 1,
		.flag = NULL,
		.val = 's',
	},

	[OPT_DEST_CONTAINER] = {
		.name = "dest",
		.has_arg = 1,
		.flag = NULL,
		.val = 'd',
	},

	{ 0 },
};

C_ASSERT(ARRAY_SIZE(getopt_long_options) == NUM_CMD_LINE_OPTIONS + 1);

static void print_unexpected_options_error(uint32_t options_mask)
{
#	define PRINT_OPTION(_opt_mask, _opt_index) \
	do {								\
		if (options_mask & (_opt_mask)) {			\
			fprintf(stderr, "\t%c, %s\n",			\
				getopt_long_options[_opt_index].val,	\
				getopt_long_options[_opt_index].name);  \
		}							\
	} while (0)

	ERROR_PRINTF("Invalid options:\n");
	PRINT_OPTION(OPT_HELP_MASK, OPT_HELP);
	PRINT_OPTION(OPT_VERSION_MASK, OPT_VERSION);
	PRINT_OPTION(OPT_RESOURCES_MASK, OPT_RESOURCES);
	PRINT_OPTION(OPT_CONTAINER_MASK, OPT_CONTAINER);
	PRINT_OPTION(OPT_SOURCE_CONTAINER_MASK, OPT_SOURCE_CONTAINER);
	PRINT_OPTION(OPT_DEST_CONTAINER_MASK, OPT_DEST_CONTAINER);

#	undef PRINT_OPTION
}

static int parse_object_name(const char *obj_name, char *expected_obj_type,
			     uint32_t *obj_id, char *obj_type)
{
	int n;

	n = sscanf(obj_name, "%u.%" STRINGIFY(OBJ_TYPE_MAX_LENGTH) "s",
		   obj_id, obj_type);
	if (n != 2) {
		ERROR_PRINTF("Invalid MC object name: %s\n", obj_name);
		return -EINVAL;
	}

	if (expected_obj_type != NULL && strcmp(obj_type, expected_obj_type) != 0) {
		ERROR_PRINTF("Expected \'%s\' object type\n", expected_obj_type);
		return -EINVAL;
	}

	return 0;
}


static void print_usage(void)
{
	static const char usage_msg[] =
		"resman [OPTION]... <command> [ARG]...\n"
		"\n"
		"General options:\n"
		"-h, --help\tPrint this message\n"
		"-v, --version\tPrint version of the resman tool\n"
		"\n"
		"Commands:\n"
		"list\tList all containers (DPRC objects) in the system.\n"
		"show [-r] <container>\n"
		"\tDisplay the contents of a DPRC/container\n"
		"\tOptions:\n"
		"\t-r, --resources\n"
		"\t\tDisplay resources instead of objects\n"
		"\tNOTE: Use 0.dprc for the global container.\n"
		"info <object>\n"
		"\tShow general info about an MC object.\n"
		"create <object type> [-c]\n"
		"\tCreate a new MC object of the given type.\n"
		"\tOptions:\n"
		"\t-c <container>, --container=<container>\n"
		"\t\tContainer in which the object is to be created\n"
		"destroy <object>\n"
		"\tDestroy an MC object.\n"
		"move <object> -s <source container> -d <destination container>\n"
		"\tMove a non-DPRC MC object from one container to another.\n"
		"\tOptions:\n"
		"\t-s <container>, --source=<container>\n"
		"\t\tContainer in which the object currently exists (source)\n"
		"\t-d <container>, --dest=<container>\n"
		"\t\tContainer to which object is to be moved (destination)\n"
		"\tNOTE: source and destination containers must have parent-child relationship.\n"
		"\n";

	printf(usage_msg);

	resman.cmd_line_options_mask &= ~OPT_HELP_MASK;
	if (resman.cmd_line_options_mask != 0)
		ERROR_PRINTF("Extra options ignored\n");
}

static void print_version(void)
{
	printf("Freescale MC resman tool version %s\n", resman_version);

	resman.cmd_line_options_mask &= ~OPT_VERSION_MASK;
	if (resman.cmd_line_options_mask != 0)
		ERROR_PRINTF("Extra options ignored\n");
}

static int open_dprc(uint32_t dprc_id, uint16_t *dprc_handle)
{
	int error;

	error = dprc_open(&resman.mc_portal,
			  dprc_id,
			  dprc_handle);
	if (error < 0) {
		ERROR_PRINTF(
			"dprc_open() failed for %u.dprc with error %d\n",
			dprc_id, error);
		goto out;
	}

	if (*dprc_handle == 0) {
		ERROR_PRINTF("dprc_open() returned invalid handle (auth 0) for "
			     "%u.dprc\n", dprc_id);

		(void)dprc_close(&resman.mc_portal, *dprc_handle);
		error = -ENOENT;
		goto out;
	}

	error = 0;
out:
	return error;
}

/**
 * Lists nested DPRCs inside a given DPRC, recursively
 */
static int list_dprc(uint32_t dprc_id, uint16_t dprc_handle,
		     int nesting_level, bool show_non_dprc_objects)
{
	int num_child_devices;
	int error = 0;

	assert(nesting_level <= MAX_DPRC_NESTING);

	for (int i = 0; i < nesting_level; i++)
		printf("  ");

	printf("%u.dprc\n", dprc_id);

	error = dprc_get_obj_count(&resman.mc_portal,
				   dprc_handle,
				   &num_child_devices);
	if (error < 0) {
		ERROR_PRINTF("dprc_get_object_count() failed with error %d\n",
			     error);
		goto out;
	}

	for (int i = 0; i < num_child_devices; i++) {
		struct dprc_obj_desc obj_desc;
		uint16_t child_dprc_handle;
		int error2;

		error = dprc_get_obj(
				&resman.mc_portal,
				dprc_handle,
				i,
				&obj_desc);
		if (error < 0) {
			ERROR_PRINTF(
				"dprc_get_object(%u) failed with error %d\n",
				i, error);
			goto out;
		}

		if (strcmp(obj_desc.type, "dprc") != 0) {
			if (show_non_dprc_objects) {
				for (int i = 0; i < nesting_level + 1; i++)
					printf("  ");

				printf("%u.%s\n", obj_desc.id, obj_desc.type);
			}

			continue;
		}

		error = open_dprc(obj_desc.id, &child_dprc_handle);
		if (error < 0)
			goto out;

		error = list_dprc(obj_desc.id, child_dprc_handle,
				  nesting_level + 1, show_non_dprc_objects);

		error2 = dprc_close(&resman.mc_portal, child_dprc_handle);
		if (error2 < 0) {
			ERROR_PRINTF("dprc_close() failed with error %d\n",
				     error2);
			if (error == 0)
				error = error2;

			goto out;
		}
	}

out:
	return error;
}

static int cmd_list_containers(void)
{
	int error;

	if (resman.num_cmd_args != 0) {
		ERROR_PRINTF("Unexpected arguments\n");
		error = -EINVAL;
		goto out;
	}

	if (resman.cmd_line_options_mask != 0) {
		print_unexpected_options_error(
			resman.cmd_line_options_mask);
		error = -EINVAL;
		goto out;
	}

	error = list_dprc(resman.root_dprc_id, resman.root_dprc_handle, 0, false);
out:
	return error;
}

static int cmd_list_one_resource_type(uint16_t dprc_handle,
				      const char *mc_res_type)
{
	int res_count;
	int res_discovered_count;
	struct dprc_res_ids_range_desc range_desc;
	int error = 0;

	error = dprc_get_res_count(&resman.mc_portal, dprc_handle,
				   (char *)mc_res_type, &res_count);
	if (error < 0) {
		ERROR_PRINTF("dprc_get_res_count() failed: %d\n", error);
		goto out;
	}

	if (res_count == 0)
		goto out;

	memset(&range_desc, 0, sizeof(struct dprc_res_ids_range_desc));
	res_discovered_count = 0;
	do {
		int id;

		error = dprc_get_res_ids(&resman.mc_portal, dprc_handle,
					 (char *)mc_res_type, &range_desc);
		if (error < 0) {
			ERROR_PRINTF("dprc_get_res_ids() failed: %d\n", error);
			goto out;
		}

		for (id = range_desc.base_id; id <= range_desc.last_id; id++) {
			printf("%d.%s\n", id, mc_res_type);
			res_discovered_count++;
		}
	} while (res_discovered_count < res_count &&
		 range_desc.iter_status != DPRC_ITER_STATUS_LAST);
out:
	return error;
}

/**
 * List resources of all types found in the container specified by dprc_handle
 */
static int list_mc_resources(uint16_t dprc_handle)
{
	/*
	 * TODO: Remove this and use dprc_get_pool_count()/dprc_get_pool() to discover
	 * the resource types at run time
	 */
	static const char *mc_res_types[] = {
		"mcp",
		"swp",
		"bp",
		"swpch",
		"fq",
		"qpr",
		"qd",
		"cg",
		"cqch",
		"rplr",
		"ifp.wr0",
		/*"port.dpsw0",*/
		"kp.wr0.ctlue",
		"kp.wr0.ctlui",
		"kp.aiop0.ctlu",
		"kp.aiop0.mflu",
		/*"plp",*/
		"prp.wr0.ctlue",
		"prp.wr0.ctlui",
		"prp.aiop0.ctlu",
		"prp.aiop0.mflu",
		/*"pp",*/
		"plcy.wr0.ctlui",
		"plcye.wr0.ctlui",
		"dpni",
		"dpcon",
		"dpci",
		"dpseci",
		"dpio",
		"dpbp",
		"dpsw",
		"dpdmux",
		"dpmac",
		"dprc",
	};

	int error;

	if (resman.cmd_line_options_mask != 0) {
		print_unexpected_options_error(resman.cmd_line_options_mask);
		error = -EINVAL;
		goto out;
	}

	for (unsigned int i = 0; i < ARRAY_SIZE(mc_res_types); i++) {
		error = cmd_list_one_resource_type(dprc_handle, mc_res_types[i]);
		if (error < 0)
			goto out;
	}

	error = 0;
out:
	return error;
}

static int list_mc_objects(uint16_t dprc_handle, char *dprc_name)
{
	int num_child_devices;
	int error;

	error = dprc_get_obj_count(&resman.mc_portal,
				   dprc_handle,
				   &num_child_devices);
	if (error < 0) {
		ERROR_PRINTF("dprc_get_object_count() failed with error %d\n",
			     error);
		goto out;
	}

	printf("%s contains %u objects%c\n", dprc_name, num_child_devices,
	       num_child_devices == 0 ? '.' : ':');

	for (int i = 0; i < num_child_devices; i++) {
		struct dprc_obj_desc obj_desc;

		error = dprc_get_obj(&resman.mc_portal,
				     dprc_handle,
				     i,
				     &obj_desc);
		if (error < 0) {
			ERROR_PRINTF(
				"dprc_get_object(%u) failed with error %d\n",
				i, error);
			goto out;
		}

		printf("%u.%s\n", obj_desc.id, obj_desc.type);
	}

	error = 0;
out:
	return error;
}

static int cmd_show_container(void)
{
	uint32_t dprc_id;
	uint16_t dprc_handle;
	char *dprc_name;
	int error = 0;
	bool dprc_opened = false;
	char obj_type[OBJ_TYPE_MAX_LENGTH + 1];

	if (resman.num_cmd_args == 0) {
		ERROR_PRINTF("<container> argument missing\n");
		error = -EINVAL;
		goto out;
	}

	if (resman.num_cmd_args > 1) {
		ERROR_PRINTF("Invalid number of arguments: %d\n",
			     resman.num_cmd_args);
		error = -EINVAL;
		goto out;
	}

	dprc_name = resman.cmd_args[0];
	error = parse_object_name(dprc_name, "dprc", &dprc_id, obj_type);
	if (error < 0)
		goto out;

	if (dprc_id != resman.root_dprc_id) {
		error = open_dprc(dprc_id, &dprc_handle);
		if (error < 0)
			goto out;

		dprc_opened = true;
	} else {
		dprc_handle = resman.root_dprc_handle;
	}

	if (resman.cmd_line_options_mask & OPT_RESOURCES_MASK) {
		resman.cmd_line_options_mask &= ~OPT_RESOURCES_MASK;
		error = list_mc_resources(dprc_handle);
	} else {
		if (resman.cmd_line_options_mask != 0) {
			print_unexpected_options_error(
				resman.cmd_line_options_mask);
			error = -EINVAL;
			goto out;
		}

		error = list_mc_objects(dprc_handle, dprc_name);
	}

out:
	if (dprc_opened) {
		int error2;

		error2 = dprc_close(&resman.mc_portal, dprc_handle);
		if (error2 < 0) {
			ERROR_PRINTF("dprc_close() failed with error %d\n",
				     error2);
			if (error == 0)
				error = error2;
		}
	}

	return error;
}

static int show_dprc_info(uint32_t dprc_id)
{
	uint16_t dprc_handle;
	int error;
	struct dprc_attributes dprc_attr;
	bool dprc_opened = false;

	if (dprc_id != resman.root_dprc_id) {
		error = open_dprc(dprc_id, &dprc_handle);
		if (error < 0)
			goto out;

		dprc_opened = true;
	} else {
		dprc_handle = resman.root_dprc_handle;
	}

	memset(&dprc_attr, 0, sizeof(dprc_attr));
	error = dprc_get_attributes(&resman.mc_portal, dprc_handle, &dprc_attr);
	if (error < 0) {
		ERROR_PRINTF("dprc_get_attributes() failed: %d\n", error);
		goto out;
	}

	assert(dprc_id == (uint32_t)dprc_attr.container_id);
	printf(
		"container id: %d\n"
		"icid: %u\n"
		"portal id: %d\n"
		"options: %#llx\n"
		"version: %u.%u\n",
		dprc_attr.container_id,
		dprc_attr.icid,
		dprc_attr.portal_id,
		(unsigned long long)dprc_attr.options,
		dprc_attr.version.major,
		dprc_attr.version.minor);

	error = 0;
out:
	if (dprc_opened) {
		int error2;

		error2 = dprc_close(&resman.mc_portal, dprc_handle);
		if (error2 < 0) {
			ERROR_PRINTF("dprc_close() failed with error %d\n",
				     error2);
			if (error == 0)
				error = error2;
		}
	}

	return error;
}

static int cmd_info_object(void)
{
	int error;
	char *obj_name;
	char obj_type[OBJ_TYPE_MAX_LENGTH + 1];
	uint32_t dprc_id;

	if (resman.num_cmd_args == 0) {
		ERROR_PRINTF("<object> argument missing\n");
		error = -EINVAL;
		goto out;
	}

	if (resman.num_cmd_args > 1) {
		ERROR_PRINTF("Invalid number of arguments: %d\n",
			     resman.num_cmd_args);
		error = -EINVAL;
		goto out;
	}

	if (resman.cmd_line_options_mask != 0) {
		print_unexpected_options_error(resman.cmd_line_options_mask);
		error = -EINVAL;
		goto out;
	}

	obj_name = resman.cmd_args[0];
	error = parse_object_name(obj_name, NULL, &dprc_id, obj_type);
	if (error < 0)
		goto out;

	if (strcmp(obj_type, "dprc") == 0) {
		error = show_dprc_info(dprc_id);
	} else {
		ERROR_PRINTF("Unexpected object type '\%s\'\n", obj_type);
		error = -EINVAL;
	}
out:
	return error;
}

/**
 * Create a DPRC object in the MC, as a child of the container
 * referred by 'dprc_handle'.
 */
static int create_dprc(uint16_t dprc_handle)
{
	int error;
	int error2;
	struct dprc_cfg cfg;
	int child_container_id;
	uint64_t mc_portal_phys_addr;
	uint32_t portal_id;
	bool portal_allocated = false;
	bool child_dprc_created = false;

	error = ioctl(resman.mc_portal.fd, RESMAN_ALLOCATE_MC_PORTAL,
		      &portal_id);
	if (error == -1) {
		error = -errno;
		ERROR_PRINTF("ioctl(RESMAN_ALLOCATE_MC_PORTAL) failed\n");
		goto error;
	}

	portal_allocated = true;
	DEBUG_PRINTF("ioctl returned portal_id: %u\n", portal_id);

	cfg.icid = DPRC_GET_ICID_FROM_POOL;
	cfg.portal_id = portal_id;
	cfg.options =
		(DPRC_CFG_OPT_SPAWN_ALLOWED | DPRC_CFG_OPT_ALLOC_ALLOWED);
	error = dprc_create_container(
			&resman.mc_portal,
			dprc_handle,
			&cfg,
			&child_container_id,
			&mc_portal_phys_addr);
	if (error < 0) {
		ERROR_PRINTF(
			"dprc_create_container() failed: %d\n", error);
		goto error;
	}

	child_dprc_created = true;
	printf("%u.dprc object created (MC portal %#llx consumed in the MC)\n",
	       child_container_id, (unsigned long long)mc_portal_phys_addr);

	error = ioctl(resman.mc_portal.fd, RESMAN_RESCAN_ROOT_DPRC, NULL);
	if (error == -1) {
		error = -errno;
		ERROR_PRINTF("ioctl(RESMAN_RESCAN_ROOT_DPRC) failed\n");
		goto error;
	}

	return 0;
error:
	if (child_dprc_created) {
		error2 = dprc_destroy_container(&resman.mc_portal, dprc_handle,
						child_container_id);
		if (error2 == -1) {
			ERROR_PRINTF(
			    "dprc_destroy_container() failed with error %d\n",
			    error);
		}
	}

	if (portal_allocated) {
		error2 = ioctl(resman.mc_portal.fd, RESMAN_FREE_MC_PORTAL,
			       portal_id);
		if (error2 == -1)
			ERROR_PRINTF("ioctl(RESMAN_FREE_MC_PORTAL) failed\n");
	}

	return error;
}

static int cmd_create_object(void)
{
	uint16_t dprc_handle;
	int error;
	char *target_obj_type;
	bool dprc_opened = false;

	if (resman.num_cmd_args == 0) {
		ERROR_PRINTF("<object type> argument missing\n");
		error = -EINVAL;
		goto out;
	}

	if (resman.num_cmd_args > 1) {
		ERROR_PRINTF("Invalid number of arguments: %d\n",
			     resman.num_cmd_args);
		error = -EINVAL;
		goto out;
	}

	target_obj_type = resman.cmd_args[0];
	if (resman.cmd_line_options_mask & OPT_CONTAINER_MASK) {
		char obj_type[OBJ_TYPE_MAX_LENGTH + 1];
		uint32_t dprc_id;

		assert(resman.cmd_line_option_arg[OPT_CONTAINER] != NULL);
		error = parse_object_name(resman.cmd_line_option_arg[OPT_CONTAINER],
					  "dprc", &dprc_id, obj_type);
		if (error < 0)
			goto out;

		if (dprc_id != resman.root_dprc_id) {
			error = open_dprc(dprc_id, &dprc_handle);
			if (error < 0)
				goto out;

			dprc_opened = true;
		} else {
			dprc_handle = resman.root_dprc_handle;
		}

		resman.cmd_line_options_mask &= ~OPT_CONTAINER_MASK;
	} else {
		dprc_handle = resman.root_dprc_handle;
	}

	if (resman.cmd_line_options_mask != 0) {
		print_unexpected_options_error(resman.cmd_line_options_mask);
		error = -EINVAL;
		goto out;
	}

	if (strcmp(target_obj_type, "dprc") == 0) {
		error = create_dprc(dprc_handle);
	} else {
		ERROR_PRINTF("Unexpected object type '\%s\'\n", target_obj_type);
		error = -EINVAL;
	}
out:
	if (dprc_opened) {
		int error2;

		error2 = dprc_close(&resman.mc_portal, dprc_handle);
		if (error2 < 0) {
			ERROR_PRINTF("dprc_close() failed with error %d\n",
				     error2);
			if (error == 0)
				error = error2;
		}
	}

	return error;
}

static int cmd_destroy_object(void)
{
	ERROR_PRINTF("command '\%s\' not implemented yet\n", resman.cmd_name);
	return ENOTSUP;
}

static int cmd_move_object(void)
{
	ERROR_PRINTF("command '\%s\' not implemented yet\n", resman.cmd_name);
	return ENOTSUP;
}

static const struct resman_command resman_commands[] = {
	{ "list", cmd_list_containers },
	{ "show", cmd_show_container },
	{ "info", cmd_info_object },
	{ "create", cmd_create_object },
	{ "destroy", cmd_destroy_object },
	{ "move", cmd_move_object },
};

static int parse_cmd_line(int argc, char *argv[])
{
	int c;
	int error = 0;
	resman_cmd_func_t *cmd_func = NULL;

	for (;;) {
		c = getopt_long(argc, argv, "hvrc:s:d:", getopt_long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'h':
			resman.cmd_line_options_mask |= OPT_HELP_MASK;
			break;

		case 'v':
			resman.cmd_line_options_mask |= OPT_VERSION_MASK;
			break;

		case 'r':
			resman.cmd_line_options_mask |= OPT_RESOURCES_MASK;
			break;

		case 'c':
			resman.cmd_line_options_mask |= OPT_CONTAINER_MASK;
			resman.cmd_line_option_arg[OPT_CONTAINER] = optarg;
			break;

		case 's':
			resman.cmd_line_options_mask |= OPT_SOURCE_CONTAINER_MASK;
			resman.cmd_line_option_arg[OPT_SOURCE_CONTAINER] = optarg;
			break;

		case 'd':
			resman.cmd_line_options_mask |= OPT_DEST_CONTAINER_MASK;
			resman.cmd_line_option_arg[OPT_DEST_CONTAINER] = optarg;
			break;

		case '?':
			error = -EINVAL;
			goto out;
	       default:
			assert(false);
		}
	}

	if (resman.cmd_line_options_mask & OPT_HELP_MASK) {
		print_usage();
		goto out;
	}

	if (resman.cmd_line_options_mask & OPT_VERSION_MASK) {
		print_version();
		goto out;
	}


	if (optind == argc) {
		ERROR_PRINTF("resman command missing\n");
		error = -EINVAL;
		goto out;
	}

	assert(optind < argc);
	resman.cmd_name = argv[optind];
	resman.num_cmd_args = argc - (optind + 1);
	if (resman.num_cmd_args != 0)
		resman.cmd_args = &argv[optind + 1];

	/*
	 * Lookup command:
	 */
	for (unsigned int i = 0; i < ARRAY_SIZE(resman_commands); i++) {
		if (strcmp(resman.cmd_name, resman_commands[i].name) == 0) {
			cmd_func = resman_commands[i].func;
			break;
		}
	}

	if (cmd_func == NULL) {
		ERROR_PRINTF("Invalid command \'%s\'\n", resman.cmd_name);
		error = -EINVAL;
		goto out;
	}

	/*
	 * Execute command:
	 */
	error = cmd_func();
out:
	return error;
}

int main(int argc, char *argv[])
{
	int error;
	bool mc_portal_initialized = false;
	bool root_dprc_opened = false;
	struct ioctl_dprc_info root_dprc_info = { 0 };

	DEBUG_PRINTF("resman built on " __DATE__ " " __TIME__ "\n");
	error = mc_portal_wrapper_init(&resman.mc_portal);
	if (error != 0)
		goto out;

	mc_portal_initialized = true;
	DEBUG_PRINTF("mc_portal regs: %p\n", resman.mc_portal.mmio_regs);
	DEBUG_PRINTF("calling ioctl\n");
	error = ioctl(resman.mc_portal.fd, RESMAN_GET_ROOT_DPRC_INFO,
		      &root_dprc_info);
	if (error == -1) {
		error = -errno;
		goto out;
	}

	DEBUG_PRINTF("ioctl returned dprc_id: %#x, dprc_handle: %#x\n",
		     root_dprc_info.dprc_id,
		     root_dprc_info.dprc_handle);

	resman.root_dprc_id = root_dprc_info.dprc_id;
	error = open_dprc(resman.root_dprc_id, &resman.root_dprc_handle);
	if (error < 0)
		goto out;

	root_dprc_opened = true;
	error = parse_cmd_line(argc, argv);
out:
	if (root_dprc_opened) {
		int error2;

		error2 = dprc_close(&resman.mc_portal, resman.root_dprc_handle);
		if (error2 < 0) {
			ERROR_PRINTF("dprc_close() failed with error %d\n",
				     error2);
			if (error == 0)
				error = error2;
		}
	}

	if (mc_portal_initialized)
		mc_portal_wrapper_cleanup(&resman.mc_portal);

	return error;
}


// SPDX-License-Identifier: GPL-2.0-or-later
/* Wrap SANE's virtual backend to reproduce process-local airscan device IDs. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sane/sane.h>
#include <sane/saneopts.h>
#include <string.h>

SANE_Status
sane_get_devices (const SANE_Device ***devices, SANE_Bool local_only)
{
	static const SANE_Device device = { "airscan:e7:Virtual scanner", "Test", "Virtual scanner", "flatbed" };
	static const SANE_Device *list[] = { &device, NULL };
	SANE_Status (*real_get_devices) (const SANE_Device ***, SANE_Bool) = dlsym (RTLD_NEXT, "sane_get_devices");
	SANE_Status status = real_get_devices (devices, local_only);
	if (status == SANE_STATUS_GOOD)
		*devices = list;
	return status;
}

SANE_Status
sane_open (SANE_String_Const name, SANE_Handle *handle)
{
	SANE_Status (*real_open) (SANE_String_Const, SANE_Handle *) = dlsym (RTLD_NEXT, "sane_open");
	if (strcmp (name, "airscan:e7:Virtual scanner") != 0)
		return SANE_STATUS_INVAL;
	return real_open ("test:0", handle);
}

const SANE_Option_Descriptor *
sane_get_option_descriptor (SANE_Handle handle, SANE_Int index)
{
	const SANE_Option_Descriptor *(*real_descriptor) (SANE_Handle, SANE_Int) = dlsym (RTLD_NEXT, "sane_get_option_descriptor");
	const SANE_Option_Descriptor *option = real_descriptor (handle, index);
	if (option && option->name && strcmp (option->name, SANE_NAME_SCAN_RESOLUTION) == 0) {
		static SANE_Option_Descriptor resolution;
		static const SANE_Word values[] = { 4, SANE_FIX (100), SANE_FIX (200), SANE_FIX (300), SANE_FIX (600) };
		resolution = *option;
		resolution.constraint_type = SANE_CONSTRAINT_WORD_LIST;
		resolution.constraint.word_list = values;
		return &resolution;
	}
	return option;
}

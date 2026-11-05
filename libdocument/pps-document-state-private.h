// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

typedef enum {
	PPS_DOCUMENT_STATE_MODIFIED = 1 << 0,
	PPS_DOCUMENT_STATE_FORMS_MODIFIED = 1 << 1,
	PPS_DOCUMENT_STATE_ANNOTATIONS_MODIFIED = 1 << 2,
} PpsDocumentState;

/* 
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005/2006, Anthony Minessale II <anthmct@yahoo.com>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthmct@yahoo.com>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * 
 * Anthony Minessale II <anthmct@yahoo.com>
 *
 *
 * switch_buffer.c -- Data Buffering Code
 *
 */
#include <switch.h>
#include <switch_buffer.h>

static uint32_t buffer_id = 0;

typedef enum {
	SWITCH_BUFFER_FLAG_DYNAMIC = (1 <<  0)
} switch_buffer_flag_t;

struct switch_buffer {
	switch_byte_t *data;
	switch_byte_t *head;
	switch_size_t used;
	switch_size_t actually_used;
	switch_size_t datalen;
	switch_size_t max_len;
	switch_size_t blocksize;
	uint32_t flags;
	uint32_t id;
};

SWITCH_DECLARE(switch_status_t) switch_buffer_create(switch_memory_pool_t *pool, switch_buffer_t **buffer, switch_size_t max_len)
{
	switch_buffer_t *new_buffer;

	if ((new_buffer = switch_core_alloc(pool, sizeof(switch_buffer_t))) != 0
		&& (new_buffer->data = switch_core_alloc(pool, max_len)) != 0) {
		new_buffer->datalen = max_len;
		new_buffer->id = buffer_id++;
		new_buffer->head = new_buffer->data;
		*buffer = new_buffer;
		return SWITCH_STATUS_SUCCESS;
	}
	return SWITCH_STATUS_MEMERR;
}

SWITCH_DECLARE(switch_status_t) switch_buffer_create_dynamic(switch_buffer_t **buffer,
															 switch_size_t blocksize,
															 switch_size_t start_len,
															 switch_size_t max_len)
{
	switch_buffer_t *new_buffer;

	if ((new_buffer = malloc(sizeof(*new_buffer)))) {
		memset(new_buffer, 0, sizeof(*new_buffer));

		if (start_len) {
			if (!(new_buffer->data = malloc(start_len))) {
				free(new_buffer);
				return SWITCH_STATUS_MEMERR;
			}
			memset(new_buffer->data, 0, start_len);
		}

		new_buffer->max_len = max_len;
		new_buffer->datalen = start_len;
		new_buffer->id = buffer_id++;
		new_buffer->blocksize = blocksize;
		new_buffer->head = new_buffer->data;
		switch_set_flag(new_buffer, SWITCH_BUFFER_FLAG_DYNAMIC);
		
		*buffer = new_buffer;
		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_MEMERR;
}

SWITCH_DECLARE(switch_size_t) switch_buffer_len(switch_buffer_t *buffer)
{

	assert(buffer != NULL);

	return buffer->datalen;

}


SWITCH_DECLARE(switch_size_t) switch_buffer_freespace(switch_buffer_t *buffer)
{
	assert(buffer != NULL);

	if (switch_test_flag(buffer, SWITCH_BUFFER_FLAG_DYNAMIC)) {
		if (buffer->max_len) {
			return (switch_size_t) (buffer->max_len - buffer->used);
		}
		return 1000000;
	}

	return (switch_size_t) (buffer->datalen - buffer->used);

}

SWITCH_DECLARE(switch_size_t) switch_buffer_inuse(switch_buffer_t *buffer)
{
	assert(buffer != NULL);

	return buffer->used;
}

SWITCH_DECLARE(switch_size_t) switch_buffer_toss(switch_buffer_t *buffer, switch_size_t datalen)
{
	switch_size_t reading = 0;

	assert(buffer != NULL);

	if (buffer->used < 1) {
		buffer->used = 0;
		return 0;
	} else if (buffer->used >= datalen) {
		reading = datalen;
	} else {
		reading = buffer->used;
	}

    buffer->used -= reading;
    buffer->head += reading;

	return buffer->used;
}

SWITCH_DECLARE(switch_size_t) switch_buffer_read(switch_buffer_t *buffer, void *data, switch_size_t datalen)
{
	switch_size_t reading = 0;

	assert(buffer != NULL);
	assert(data != NULL);


	if (buffer->used < 1) {
		buffer->used = 0;
		return 0;
	} else if (buffer->used >= datalen) {
		reading = datalen;
	} else {
		reading = buffer->used;
	}

	memcpy(data, buffer->head, reading);
	buffer->used -= reading;
	buffer->head += reading;

	//if (buffer->id == 4) printf("%u o %d = %d\n", buffer->id, (uint32_t)reading, (uint32_t)buffer->used);
	return reading;
}

SWITCH_DECLARE(switch_size_t) switch_buffer_write(switch_buffer_t *buffer, void *data, switch_size_t datalen)
{
	switch_size_t freespace, actual_freespace;

	assert(buffer != NULL);
	assert(data != NULL);
	assert(buffer->data != NULL);
	
	if (!datalen) {
		return buffer->used;
	}

	actual_freespace = buffer->datalen - buffer->actually_used;

	if (actual_freespace < datalen) {
		memmove(buffer->data, buffer->head, buffer->used);
		buffer->head = buffer->data;
		buffer->actually_used = buffer->used;
	}

	freespace = buffer->datalen - buffer->used;

	/*
	if (buffer->data != buffer->head) {
		memmove(buffer->data, buffer->head, buffer->used);
		buffer->head = buffer->data;
	}
	*/
	if (switch_test_flag(buffer, SWITCH_BUFFER_FLAG_DYNAMIC)) {
		if (freespace < datalen) {
			switch_size_t new_size, new_block_size;

			new_size = buffer->datalen + datalen;
			new_block_size = buffer->datalen + buffer->blocksize;

			if (new_block_size > new_size) {
				new_size = new_block_size;
			}
			buffer->head = buffer->data;
			if (!(buffer->data = realloc(buffer->data, new_size))) {
				return 0;
			}
			buffer->head = buffer->data;
			buffer->datalen = new_size;
		}
	}
	
	freespace = buffer->datalen - buffer->used;

	if (freespace < datalen) {
		return 0;
	} else {
		memcpy(buffer->head + buffer->used, data, datalen);
		buffer->used += datalen;
		buffer->actually_used += datalen;
	}
	//if (buffer->id == 4) printf("%u i %d = %d\n", buffer->id, (uint32_t)datalen, (uint32_t)buffer->used);

	return buffer->used;
}

SWITCH_DECLARE(void) switch_buffer_zero(switch_buffer_t *buffer)
{
	assert(buffer != NULL);
    assert(buffer->data != NULL);

	buffer->used = 0;
	buffer->head = buffer->data;
}

SWITCH_DECLARE(void) switch_buffer_destroy(switch_buffer_t **buffer)
{
	if (*buffer && switch_test_flag((*buffer), SWITCH_BUFFER_FLAG_DYNAMIC)) {
		free((*buffer)->data);
		free(*buffer);
	}

	*buffer = NULL;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:nil
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 expandtab:
 */

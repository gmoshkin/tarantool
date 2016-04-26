/*
 * Copyright 2010-2015, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "sophia_engine.h"
#include "sophia_space.h"
#include "sophia_index.h"
#include "say.h"
#include "tuple.h"
#include "tuple_update.h"
#include "scoped_guard.h"
#include "schema.h"
#include "space.h"
#include "txn.h"
#include "cfg.h"
#include "sophia.h"
#include <stdio.h>
#include <inttypes.h>
#include <bit/bit.h> /* load/store */

static uint64_t num_parts[8];

void*
SophiaIndex::createDocument(const char *key, const char **keyend)
{
	assert(key_def->part_count <= 8);
	void *obj = sp_document(db);
	if (obj == NULL)
		sophia_error(env);
	sp_setstring(obj, "arg", fiber(), 0);
	if (key == NULL)
		return obj;
	uint32_t i = 0;
	while (i < key_def->part_count) {
		char partname[32];
		snprintf(partname, sizeof(partname), "key_%" PRIu32, i);
		const char *part;
		uint32_t partsize;
		if (key_def->parts[i].type == STRING) {
			part = mp_decode_str(&key, &partsize);
		} else {
			num_parts[i] = mp_decode_uint(&key);
			part = (char *)&num_parts[i];
			partsize = sizeof(uint64_t);
		}
		if (partsize == 0)
			part = "";
		if (sp_setstring(obj, partname, part, partsize) == -1)
			sophia_error(env);
		i++;
	}
	if (keyend) {
		*keyend = key;
	}
	return obj;
}

static inline void*
sophia_configure_storage(struct space *space, struct key_def *key_def)
{
	SophiaEngine *engine =
		(SophiaEngine *)space->handler->engine;
	void *env = engine->env;
	/* create database */
	char c[128];
	snprintf(c, sizeof(c), "%" PRIu32 ":%" PRIu32,
	         key_def->space_id, key_def->iid);
	sp_setstring(env, "db", c, 0);
	/* define storage scheme */
	uint32_t i = 0;
	while (i < key_def->part_count)
	{
		/* create key field */
		char part[32];
		snprintf(c, sizeof(c), "db.%" PRIu32 ":%" PRIu32 ".scheme",
		         key_def->space_id, key_def->iid);
		snprintf(part, sizeof(part), "key_%" PRIu32, i);
		sp_setstring(env, c, part, 0);
		/* set field type */
		char type[32];
		snprintf(type, sizeof(type), "%s,key(%" PRIu32 ")",
		         (key_def->parts[i].type == NUM ? "u64" : "string"),
		         i);
		snprintf(c, sizeof(c), "db.%" PRIu32 ":%" PRIu32 ".scheme.%s",
		         key_def->space_id, key_def->iid, part);
		sp_setstring(env, c, type, 0);
		i++;
	}
	/* create value field */
	snprintf(c, sizeof(c), "db.%" PRIu32 ":%" PRIu32 ".scheme",
	         key_def->space_id, key_def->iid);
	sp_setstring(env, c, "value", 0);
	/* get database object */
	snprintf(c, sizeof(c), "db.%" PRIu32 ":%" PRIu32,
	         key_def->space_id, key_def->iid);
	void *db = sp_getobject(env, c);
	if (db == NULL)
		sophia_error(env);
	return db;
}

static inline void
sophia_ctl(char *path, int size, struct key_def *key_def, const char *name)
{
	snprintf(path, size, "db.%" PRIu32 ":%" PRIu32 ".%s",
	         key_def->space_id, key_def->iid, name);
}

static inline void
sophia_configure(struct space *space, struct key_def *key_def)
{
	SophiaEngine *engine =
		(SophiaEngine *)space->handler->engine;
	void *env = engine->env;
	char c[128];
	/* db.id */
	sophia_ctl(c, sizeof(c), key_def, "id");
	sp_setint(env, c, key_def->space_id);
	/* db.path */
	if (key_def->opts.path[0] != '\0') {
		sophia_ctl(c, sizeof(c), key_def, "path");
		sp_setstring(env, c, key_def->opts.path, 0);
	}
	/* db.upsert */
	sophia_ctl(c, sizeof(c), key_def, "upsert");
	sp_setstring(env, c, (const void *)(uintptr_t)sophia_upsert_cb, 0);
	sophia_ctl(c, sizeof(c), key_def, "upsert_arg");
	sp_setstring(env, c, (const void *)key_def, 0);
	/* db.compression */
	if (key_def->opts.compression[0] != '\0') {
		sophia_ctl(c, sizeof(c), key_def, "compression");
		sp_setstring(env, c, key_def->opts.compression, 0);
	}
	/* db.compression_branch */
	if (key_def->opts.compression_branch[0] != '\0') {
		sophia_ctl(c, sizeof(c), key_def, "compression_branch");
		sp_setstring(env, c, key_def->opts.compression_branch, 0);
	}
	/* db.compression_key */
	sophia_ctl(c, sizeof(c), key_def, "compression_key");
	sp_setint(env, c, key_def->opts.compression_key);
	/* db.node_preload */
	sophia_ctl(c, sizeof(c), key_def, "node_preload");
	sp_setint(env, c, cfg_geti("sophia.node_preload"));
	/* db.node_size */
	sophia_ctl(c, sizeof(c), key_def, "node_size");
	sp_setint(env, c, key_def->opts.node_size);
	/* db.page_size */
	sophia_ctl(c, sizeof(c), key_def, "page_size");
	sp_setint(env, c, key_def->opts.page_size);
	/* db.mmap */
	sophia_ctl(c, sizeof(c), key_def, "mmap");
	sp_setint(env, c, cfg_geti("sophia.mmap"));
	/* db.sync */
	sophia_ctl(c, sizeof(c), key_def, "sync");
	sp_setint(env, c, cfg_geti("sophia.sync"));
	/* db.amqf */
	sophia_ctl(c, sizeof(c), key_def, "amqf");
	sp_setint(env, c, key_def->opts.amqf);
	/* db.read_oldest */
	sophia_ctl(c, sizeof(c), key_def, "read_oldest");
	sp_setint(env, c, key_def->opts.read_oldest);
	/* db.expire */
	sophia_ctl(c, sizeof(c), key_def, "expire");
	sp_setint(env, c, key_def->opts.expire);
	/* db.path_fail_on_drop */
	sophia_ctl(c, sizeof(c), key_def, "path_fail_on_drop");
	sp_setint(env, c, 0);
}

SophiaIndex::SophiaIndex(struct key_def *key_def_arg)
	: Index(key_def_arg)
{
	struct space *space = space_cache_find(key_def->space_id);
	SophiaEngine *engine =
		(SophiaEngine *)space->handler->engine;
	env = engine->env;
	int rc;
	sophia_workers_start(env);
	db = sophia_configure_storage(space, key_def);
	if (db == NULL)
		sophia_error(env);
	sophia_configure(space, key_def);
	/* start two-phase recovery for a space:
	 * a. created after snapshot recovery
	 * b. created during log recovery
	*/
	rc = sp_open(db);
	if (rc == -1)
		sophia_error(env);
	format = space->format;
	tuple_format_ref(format, 1);
}

SophiaIndex::~SophiaIndex()
{
	if (db == NULL)
		return;
	/* schedule database shutdown */
	int rc = sp_close(db);
	if (rc == -1)
		goto error;
	/* unref database object */
	rc = sp_destroy(db);
	if (rc == -1)
		goto error;
error:;
	char *error = (char *)sp_getstring(env, "sophia.error", 0);
	say_info("sophia space %" PRIu32 " close error: %s",
			 key_def->space_id, error);
	free(error);
}

size_t
SophiaIndex::size() const
{
	char c[128];
	sophia_ctl(c, sizeof(c), key_def, "index.count");
	return sp_getint(env, c);
}

size_t
SophiaIndex::bsize() const
{
	char c[128];
	sophia_ctl(c, sizeof(c), key_def, "index.memory_used");
	return sp_getint(env, c);
}

struct tuple *
SophiaIndex::findByKey(const char *key, uint32_t part_count = 0) const
{
	(void)part_count;
	void *obj = ((SophiaIndex *)this)->createDocument(key, NULL);
	void *transaction = db;
	/* engine_tx might be empty, even if we are in txn context.
	 *
	 * This can happen on a first-read statement. */
	if (in_txn())
		transaction = in_txn()->engine_tx;
	/* try to read from cache first, if nothing is found
	 * retry using disk */
	sp_setint(obj, "cache_only", 1);
	int rc;
	rc = sp_open(obj);
	if (rc == -1) {
		sp_destroy(obj);
		sophia_error(env);
	}
	void *result = sp_get(transaction, obj);
	if (result == NULL) {
		sp_setint(obj, "cache_only", 0);
		result = sophia_read(transaction, obj);
		sp_destroy(obj);
		if (result == NULL)
			return NULL;
	} else {
		sp_destroy(obj);
	}
	struct tuple *tuple = sophia_tuple_new(result, key_def, format);
	sp_destroy(result);
	return tuple;
}

struct tuple *
SophiaIndex::replace(struct tuple*, struct tuple*, enum dup_replace_mode)
{
	/* This method is unused by sophia index.
	 *
	 * see: sophia_space.cc
	*/
	assert(0);
	return NULL;
}

struct sophia_iterator {
	struct iterator base;
	const char *key;
	const char *keyend;
	struct space *space;
	struct key_def *key_def;
	void *env;
	void *db;
	void *cursor;
	void *current;
};

void
sophia_iterator_free(struct iterator *ptr)
{
	assert(ptr->free == sophia_iterator_free);
	struct sophia_iterator *it = (struct sophia_iterator *) ptr;
	if (it->current) {
		sp_destroy(it->current);
		it->current = NULL;
	}
	if (it->cursor) {
		sp_destroy(it->cursor);
		it->cursor = NULL;
	}
	free(ptr);
}

struct tuple *
sophia_iterator_last(struct iterator *ptr __attribute__((unused)))
{
	return NULL;
}

struct tuple *
sophia_iterator_next(struct iterator *ptr)
{
	struct sophia_iterator *it = (struct sophia_iterator *) ptr;
	assert(it->cursor != NULL);

	/* read from cache */
	void *obj;
	obj = sp_get(it->cursor, it->current);
	if (likely(obj != NULL)) {
		sp_destroy(it->current);
		it->current = obj;
		return sophia_tuple_new(obj, it->key_def, it->space->format);

	}
	/* switch to asynchronous mode (read from disk) */
	sp_setint(it->current, "cache_only", 0);

	obj = sophia_read(it->cursor, it->current);
	if (obj == NULL) {
		ptr->next = sophia_iterator_last;
		/* immediately close the cursor */
		sp_destroy(it->cursor);
		sp_destroy(it->current);
		it->current = NULL;
		it->cursor = NULL;
		return NULL;
	}
	sp_destroy(it->current);
	it->current = obj;

	/* switch back to synchronous mode */
	sp_setint(obj, "cache_only", 1);
	return sophia_tuple_new(obj, it->key_def, it->space->format);
}

struct tuple *
sophia_iterator_first(struct iterator *ptr)
{
	struct sophia_iterator *it = (struct sophia_iterator *) ptr;
	ptr->next = sophia_iterator_next;
	return sophia_tuple_new(it->current, it->key_def, it->space->format);
}

struct tuple *
sophia_iterator_eq(struct iterator *ptr)
{
	struct sophia_iterator *it = (struct sophia_iterator *) ptr;
	ptr->next = sophia_iterator_last;
	SophiaIndex *index = (SophiaIndex *)index_find(it->space, 0);
	return index->findByKey(it->key);
}

struct iterator *
SophiaIndex::allocIterator() const
{
	struct sophia_iterator *it =
		(struct sophia_iterator *) calloc(1, sizeof(*it));
	if (it == NULL) {
		tnt_raise(OutOfMemory, sizeof(struct sophia_iterator),
			  "Sophia Index", "iterator");
	}
	it->base.next = sophia_iterator_last;
	it->base.free = sophia_iterator_free;
	return (struct iterator *) it;
}

void
SophiaIndex::initIterator(struct iterator *ptr,
                          enum iterator_type type,
                          const char *key, uint32_t part_count) const
{
	struct sophia_iterator *it = (struct sophia_iterator *) ptr;
	assert(it->cursor == NULL);
	if (part_count > 0) {
		if (part_count != key_def->part_count) {
			tnt_raise(UnsupportedIndexFeature, this, "partial keys");
		}
	} else {
		key = NULL;
	}
	it->space = space_cache_find(key_def->space_id);
	it->key_def = key_def;
	it->key = key;
	it->env = env;
	it->db  = db;
	it->current = NULL;
	const char *compare;
	/* point-lookup iterator */
	if (type == ITER_EQ) {
		ptr->next = sophia_iterator_eq;
		return;
	}
	/* prepare for the range scan */
	switch (type) {
	case ITER_ALL:
	case ITER_GE: compare = ">=";
		break;
	case ITER_GT: compare = ">";
		break;
	case ITER_LE: compare = "<=";
		break;
	case ITER_LT: compare = "<";
		break;
	default:
		return initIterator(ptr, type, key, part_count);
	}
	it->cursor = sp_cursor(env);
	if (it->cursor == NULL)
		sophia_error(env);
	/* Position first key here, since key pointer might be
	 * unavailable from lua.
	 *
	 * Read from disk and fill cursor cache.
	 */
	SophiaIndex *index = (SophiaIndex *)this;
	void *obj = index->createDocument(key, &it->keyend);
	sp_setstring(obj, "order", compare, 0);
	obj = sophia_read(it->cursor, obj);
	if (obj == NULL) {
		sp_destroy(it->cursor);
		it->cursor = NULL;
		return;
	}
	it->current = obj;
	/* switch to sync mode (cache read) */
	sp_setint(obj, "cache_only", 1);
	ptr->next = sophia_iterator_first;
}

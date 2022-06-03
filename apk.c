// SPDX-FileCopyrightText: 2022-present Jakub Jirutka <jakub@jirutka.cz>
// SPDX-License-Identifier: GPL-2.0-only

#include <assert.h>
#include <errno.h>
#include <unistd.h>

#include <apk/apk_blob.h>
#include <apk/apk_database.h>
#include <apk/apk_defines.h>
#include <apk/apk_package.h>
#include <apk/apk_print.h>
#include <apk/apk_solver.h>

#include <json.h>  // json-c
#include <daemon/plugin.h>  // collectd

#define PLUGIN_NAME "apk"

#ifndef PLUGIN_VERSION
  #define PLUGIN_VERSION "0.1.0"
#endif

#define LOG_PREFIX PLUGIN_NAME " plugin: "

#define UNUSED __attribute__((unused))

#define log_info(...) INFO(LOG_PREFIX __VA_ARGS__)
#define log_warn(...) WARNING(LOG_PREFIX __VA_ARGS__)
#define log_err(...) ERROR(LOG_PREFIX __VA_ARGS__)

extern unsigned int apk_flags;
extern int apk_verbosity;

// Override function from libapk defined in src/print.c.
void apk_log (const char UNUSED *_prefix, const char *format, ...) {
	va_list ap;
	va_start(ap, format);

	char msg[1024];
	vsnprintf(msg, sizeof(msg), format, ap);
	va_end(ap);

	log_info("%s", msg);
}

// Override function from libapk defined in src/print.c.
void apk_log_err (const char *prefix, const char *format, ...) {
	va_list ap;
	va_start(ap, format);

	char msg[1024];
	vsnprintf(msg, sizeof(msg), format, ap);
	va_end(ap);

	if (strcmp("ERROR: ", prefix) == 0) {
		log_err("%s", msg);
	} else {
		log_warn("%s", msg);
	}
}

static int dispatch_gauge (const char *plugin_instance, const char *type,
                           gauge_t value, meta_data_t *meta) {
	value_list_t vl = {
		.plugin = PLUGIN_NAME,
		.values = &(value_t){ .gauge = value },
		.values_len = 1,
		.meta = meta,
	};
	strncpy(vl.plugin_instance, plugin_instance, sizeof(vl.plugin_instance));
	strncpy(vl.type, type, sizeof(vl.type));

	int status = plugin_dispatch_values(&vl);

	if (meta != NULL) {
		meta_data_destroy(meta);
	}
	return status;
}

static json_object *apk_change_to_json (struct apk_change *change) {
	const struct apk_package *old_pkg = change->old_pkg,
	                         *new_pkg = change->new_pkg;

	assert(old_pkg && "change.old_pkg is NULL");
	assert(old_pkg->name && "change.old_pkg.name is NULL");
	assert(new_pkg && "change.new_pkg is NULL");

	char *pkgname = old_pkg->name->name;
	char *origin = apk_blob_cstr(*old_pkg->origin);
	char *old_ver = apk_blob_cstr(*old_pkg->version);
	char *new_ver = apk_blob_cstr(*new_pkg->version);

	json_object *obj = json_object_new_object();
	json_object_object_add(obj, "p", json_object_new_string(pkgname));
	json_object_object_add(obj, "o", json_object_new_string(origin));
	json_object_object_add(obj, "v", json_object_new_string(old_ver));
	json_object_object_add(obj, "w", json_object_new_string(new_ver));

	free(old_ver);
	free(new_ver);
	free(origin);

	return obj;
}

static int apk_upgradeable_read (void) {
	int rc = -1;

	json_object *pkgs = json_object_new_array();

	struct apk_db_options db_opts = {0};
	list_init(&db_opts.repository_list);
	db_opts.open_flags = APK_OPENF_READ | APK_OPENF_NO_AUTOUPDATE;

	struct apk_database db;
	apk_db_init(&db);

	int r = 0;
	if ((r = apk_db_open(&db, &db_opts)) != 0) {
		log_err("failed to open apk database: %s", apk_error_str(r));
		goto done;
	}

	struct apk_changeset changeset = {0};
	if (apk_solver_solve(&db, APK_SOLVERF_UPGRADE, db.world, &changeset) != 0) {
		log_err("apk solver returned errors");
		goto done;
	}

	int count = 0;
	{
		struct apk_change *change;
		foreach_array_item(change, changeset.changes) {
			if (change->old_pkg != change->new_pkg) {
				count++;
				json_object_array_add(pkgs, apk_change_to_json(change));
			}
		}
		apk_change_array_free(&changeset.changes);
	}

	const char *pkgs_json = json_object_to_json_string_ext(pkgs, JSON_C_TO_STRING_PLAIN);
	log_info("packages = %s", pkgs_json);

	meta_data_t *meta = meta_data_create();
	if (meta_data_add_string(meta, "packages", pkgs_json) < 0) {
		log_err("unable to set value metadata");
		goto done;
	}

	dispatch_gauge("upgradeable", "count", count, meta);

	rc = 0;
done:
	if (db.open_complete) {
		apk_db_close(&db);
	}
	json_object_put(pkgs);

	return rc;
}

// cppcheck-suppress unusedFunction
void module_register (void) {
	// Cached APKINDEXes may be outdated and we would need root privileges to
	// update them, so better to always fetch fresh APKINDEXes in-memory.
	apk_flags = APK_NO_CACHE | APK_SIMULATE;

	INFO("registering plugin " PLUGIN_NAME " " PLUGIN_VERSION);
	plugin_register_read(PLUGIN_NAME, apk_upgradeable_read);
}

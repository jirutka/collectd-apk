// SPDX-FileCopyrightText: 2022-present Jakub Jirutka <jakub@jirutka.cz>
// SPDX-License-Identifier: GPL-2.0-or-later

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
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
  #define PLUGIN_VERSION "0.2.0"
#endif

#define OS_RELEASE_PATH "/etc/os-release"

#define LOG_PREFIX PLUGIN_NAME " plugin: "

#define log_info(...) INFO(LOG_PREFIX __VA_ARGS__)
#define log_warn(...) WARNING(LOG_PREFIX __VA_ARGS__)
#define log_err(...) ERROR(LOG_PREFIX __VA_ARGS__)

#ifndef min
  #define min(a, b) ((a) < (b) ? (a) : (b))
#endif

#define UNUSED __attribute__((unused))

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

// This is a very simplified implementation, it does not support escaping
// (`"behold \"x\" var"`) nor doubled quote character (`"dont ""do this"`).
static int parse_enclosed_word (char *dest, const char *str, size_t max_len) {
	size_t len = 0;
	const char *end = NULL;

	// /^"([^"]*)".*/ or /^'([^']*)'.*/
	if (str[0] == '"' || str[0] == '\'') {
		if (!(end = strchr(str + 1, str[0]))) {
			return -1;
		}
		len = end - ++str;
	// /^([^ \t\r\n;]*).*/
	} else {
		len = strcspn(str, " \t\r\n;");
	}
	strncpy(dest, str, min(len, max_len));

	return len;
}

struct os_release {
	char id[64];
	char version_id[64];
};

static int read_os_release (struct os_release *dest) {
	FILE *fp = NULL;

	if (!(fp = fopen(OS_RELEASE_PATH, "r"))) {
		return -1;
	}

	char line[128] = {0};
	while (fgets(line, sizeof(line), fp)) {
		char key[16] = {0};
		int pos = 0;
		if (sscanf(line, " %15[A-Za-z0-9_]=%n", key, &pos) < 1) {
		//                ^ ^-- this MUST match sizeof(key) - 1
		//                `---- allow zero or more whitespace chars at the beginning
			continue;
		}
		char *rest = line + pos;

		if (strcmp(key, "ID") == 0) {
			parse_enclosed_word(dest->id, rest, sizeof(dest->id));
		} else if (strcmp(key, "VERSION_ID") == 0) {
			parse_enclosed_word(dest->version_id, rest, sizeof(dest->version_id));
		}

	}
	fclose(fp);

	return 0;
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

	return plugin_dispatch_values(&vl);
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

static int find_upgradable_pkgs (struct apk_database *db, json_object *array) {
	assert(db && db->open_complete);
	assert(json_object_is_type(array, json_type_array));

	struct apk_changeset changeset = {0};
	if (apk_solver_solve(db, APK_SOLVERF_UPGRADE, db->world, &changeset) != 0) {
		return -1;
	}

	struct apk_change *change;
	foreach_array_item(change, changeset.changes) {
		if (change->old_pkg != change->new_pkg) {
			json_object_array_add(array, apk_change_to_json(change));
		}
	}
	apk_change_array_free(&changeset.changes);

	return 0;
}

static int apk_upgradable_read (void) {
	int rc = -1;

	json_object *pkgs = json_object_new_array();
	meta_data_t *meta = meta_data_create();

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

	if (find_upgradable_pkgs(&db, pkgs) < 0) {
		log_err("failed to find upgradable packages, apk solver returned errors");
		goto done;
	}

	const char *pkgs_json = json_object_to_json_string_ext(pkgs, JSON_C_TO_STRING_PLAIN);
	if (meta_data_add_string(meta, "packages", pkgs_json) < 0) {
		log_err("failed to add value metadata");
		goto done;
	}

	struct os_release os = { "", "" };
	if (read_os_release(&os) < 0) {
		log_warn("failed to read " OS_RELEASE_PATH ": %s", strerror(errno));
	}
	meta_data_add_string(meta, "os-id", os.id);
	meta_data_add_string(meta, "os-version", os.version_id);

	log_info("metadata: os-id = \"%s\", os-version = \"%s\", packages = %s",
	         os.id, os.version_id, pkgs_json);

	dispatch_gauge("upgradable", "count", json_object_array_length(pkgs), meta);

	rc = 0;
done:
	if (db.open_complete) {
		apk_db_close(&db);
	}
	meta_data_destroy(meta);
	json_object_put(pkgs);

	return rc;
}

// cppcheck-suppress unusedFunction
void module_register (void) {
	// Cached APKINDEXes may be outdated and we would need root privileges to
	// update them, so better to always fetch fresh APKINDEXes in-memory.
	apk_flags = APK_NO_CACHE | APK_SIMULATE;

	INFO("registering plugin " PLUGIN_NAME " " PLUGIN_VERSION);
	plugin_register_read(PLUGIN_NAME, apk_upgradable_read);
}

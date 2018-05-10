/*
 * Copyright (C) 2018 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <limits.h>
#include "fsutils.h"
#include "strutils.h"
#include "cmdutils.h"
#include "load_conf.h"
#include "sender.h"
#include "probeutils.h"
#include "android_events.h"
#include "history.h"
#include "property.h"
#include "startupreason.h"
#include "log_sys.h"

/* get_log_file_* only used to copy regular file which can be mmaped */
static void get_log_file_complete(struct log_t *log, char *desdir)
{
	char *des;
	char *name;
	int ret;

	name = log->name;

	ret = asprintf(&des, "%s/%s", desdir, name);
	if (ret < 0) {
		LOGE("compute string failed, out of memory\n");
		return;
	}

	ret = do_copy_tail(log->path, des, 0);
	if (ret < 0) {
		LOGE("copy (%s) failed\n, error (%s)\n", log->path,
		     strerror(errno));
	}

	free(des);
}

static void get_log_file_tail(struct log_t *log, char *desdir)
{
	char *des;
	char timebuf[24];
	char *name;
	char *start;
	int lines;
	int hours;
	int start_line;
	int file_lines;
	struct mm_file_t *mfile;
	int ret;

	lines = atoi(log->lines);
	name = log->name;
	get_uptime_string(timebuf, &hours);
	ret = asprintf(&des, "%s/%s_%s", desdir, name, timebuf);
	if (ret < 0) {
		LOGE("compute string failed, out of memory\n");
		return;
	}

	mfile = mmap_file(log->path);
	if (!mfile) {
		LOGE("mmap (%s) failed, error (%s)\n", log->path,
		     strerror(errno));
		goto free;
	}
	file_lines = mm_count_lines(mfile);
	if (file_lines <= 0) {
		LOGW("get lines (%s, %d) failed\n", mfile->path, file_lines);
		goto unmap;
	}
	start_line = MAX(file_lines - lines, 0) + 1;
	start = mm_get_line(mfile, start_line);
	ret = overwrite_file(des, start);
	if (ret < 0) {
		LOGE("create file with (%s, %p) failed, error (%s)\n",
		     des, start, strerror(errno));
		goto unmap;
	}

unmap:
	unmap_file(mfile);
free:
	free(des);
}

static void get_log_file(struct log_t *log, char *desdir)
{
	int lines;

	if (log->lines == NULL) {
		get_log_file_complete(log, desdir);
		return;
	}

	lines = atoi(log->lines);
	if (lines > 0)
		get_log_file_tail(log, desdir);
	else
		get_log_file_complete(log, desdir);
}

static void get_log_rotation(struct log_t *log, char *desdir)
{
	char *suffix;
	char *prefix;
	char *dir;
	char *p;
	int count;
	char *files[512];
	int number;
	int target_num = -1;
	char *target_file = NULL;
	char *name;
	int i;

	dir = strdup(log->path);
	if (!dir) {
		LOGE("compute string failed, out of memory\n");
		return;
	}

	/* dir      prefix    suffix
	 * |          |          |
	 * /tmp/hvlog/hvlog_cur.[biggest]
	 */
	p = strrchr(dir, '/');
	if (p == NULL) {
		LOGE("invalid path (%s) in log (%s), ", dir, log->name);
		LOGE("file_rotation only support absolute path\n");
		goto free_dir;
	} else {
		prefix = p + 1;
		*p = 0;
	}

	p = strstr(prefix, ".[");
	if (p == NULL) {
		LOGE("invalid path (%s) in log (%s)\n", log->path, log->name);
		goto free_dir;
	} else {
		suffix = p + 2;
		*p = 0;
	}

	p = suffix + strlen(suffix) - 1;
	if (*p == ']') {
		*p = 0;
	} else {
		LOGE("invalid path (%s) in log (%s)\n", log->path, log->name);
		goto free_dir;
	}


	struct log_t toget;

	memcpy(&toget, log, sizeof(toget));
	count = lsdir(dir, files, ARRAY_SIZE(files));
	if (count > 2) {
		for (i = 0; i < count; i++) {
			name = strrchr(files[i], '/') + 1;
			if (!strstr(name, prefix))
				continue;

			number = atoi(strrchr(name, '.') + 1);
			if (!strncmp(suffix, "biggest", 7)) {
				if (target_num == -1 ||
				    number > target_num){
					target_file = files[i];
					target_num = number;
				}
			} else if (!strncmp(suffix, "smallest", 8)) {
				if (target_num == -1 ||
				    number < target_num) {
					target_file = files[i];
					target_num = number;
				}
			} else if (!strncmp(suffix, "all", 3)) {
				toget.path = files[i];
				toget.name = name;
				get_log_file(&toget, desdir);
			}
		}
	} else if (count < 0) {
		LOGE("lsdir (%s) failed, error (%s)\n", dir,
		     strerror(-count));
		goto free;
	}

	if (!strncmp(suffix, "all", 3))
		goto free;

	if (target_file) {
		toget.path = target_file;
		get_log_file(&toget, desdir);
	} else {
		LOGW("no logs found for (%s)\n", log->name);
		goto free;
	}

free:
	while (count > 0)
		free(files[--count]);
free_dir:
	free(dir);
}

static void get_log_node(struct log_t *log, char *desdir)
{
	char *des;
	char *name;
	int ret;

	name = log->name;
	ret = asprintf(&des, "%s/%s", desdir, name);
	if (ret < 0) {
		LOGE("compute string failed, out of memory\n");
		return;
	}

	ret = do_copy_eof(log->path, des);
	if (ret < 0) {
		LOGE("copy (%s) failed, error (%s)\n", log->path,
		     strerror(errno));
		goto free;
	}

free:
	free(des);
}

static void out_via_fork(struct log_t *log, char *desdir)
{
	char *des;
	int ret;

	ret = asprintf(&des, "%s/%s", desdir, log->name);
	if (ret < 0) {
		LOGE("compute string failed, out of memory\n");
		return;
	}

	exec_out2file(des, log->path);
	free(des);
}

static void get_log_cmd(struct log_t *log, char *desdir)
{
	out_via_fork(log, desdir);
}

static void crashlog_get_log(struct log_t *log, void *data)
{

	struct sender_t *crashlog;
	unsigned long long start, end;
	int spent;
	int quota;
	char *desdir = (char *)data;

	crashlog = get_sender_by_name("crashlog");
	if (!crashlog)
		return;

	quota = atoi(crashlog->spacequota);
	if (!space_available(crashlog->outdir, quota)) {
		hist_raise_infoerror("SPACE_FULL");
		return;
	}

	start = get_uptime();
	if (!strcmp("file", log->type))
		get_log_file(log, desdir);
	else if (!strcmp("node", log->type))
		get_log_node(log, desdir);
	else if (!strcmp("cmd", log->type))
		get_log_cmd(log, desdir);
	else if (!strcmp("file_rotation", log->type))
		get_log_rotation(log, desdir);
	end = get_uptime();

	spent = (int)((end - start) / 1000000000LL);
	if (spent < 5)
		LOGD("get (%s) spend %ds\n", log->name, spent);
	else
		LOGW("get (%s) spend %ds\n", log->name, spent);
}

static void crashlog_send_crash(struct event_t *e)
{
	struct crash_t *crash;
	struct log_t *log;
	struct sender_t *crashlog;
	char *key  = NULL;
	char *trfile = NULL;
	char *data0;
	char *data1;
	char *data2;
	int id;
	int ret;
	int quota;
	struct crash_t *rcrash = (struct crash_t *)e->private;

	if (!strcmp(rcrash->trigger->type, "file"))
		ret = asprintf(&trfile, "%s", rcrash->trigger->path);
	else if (!strcmp(rcrash->trigger->type, "dir"))
		ret = asprintf(&trfile, "%s/%s", rcrash->trigger->path,
			       e->path);
	if (ret < 0) {
		LOGE("compute string failed, out of memory\n");
		return;
	}

	crash = rcrash->reclassify(rcrash, trfile, &data0, &data1, &data2);
	if (crash == NULL) {
		LOGE("reclassify crash (%s) failed\n", rcrash->name);
		goto free_trfile;
	}

	/* change the class for other senders */
	e->private = (void *)crash;
	key = generate_event_id("CRASH", crash->name);
	if (key == NULL) {
		LOGE("generate event id failed, error (%s)\n",
		     strerror(errno));
		goto free_data;
	}

	if (to_collect_logs(crash) ||
	    !strcmp(e->channel, "inotify")) {
		e->dir = generate_log_dir(MODE_CRASH, key);
		if (e->dir == NULL) {
			LOGE("generate crashlog dir failed\n");
			goto free_key;
		}

		generate_crashfile(e->dir, "CRASH", key,
				   crash->name,
				   data0, data1, data2);
		for_each_log_collect(id, log, crash) {
			if (!log)
				continue;

			log->get(log, (void *)e->dir);
		}

	}

	crashlog = get_sender_by_name("crashlog");
	if (!crashlog)
		goto free_key;

	quota = atoi(crashlog->spacequota);
	if (!space_available(crashlog->outdir, quota)) {
		hist_raise_infoerror("SPACE_FULL");
	} else if (!strcmp(e->channel, "inotify")) {
		/* get the trigger file */
		char *src;
		char *des;

		ret = asprintf(&des, "%s/%s", e->dir, e->path);
		if (ret < 0) {
			LOGE("compute string failed, out of memory\n");
			goto free_key;
		}

		ret = asprintf(&src, "%s/%s", crash->trigger->path, e->path);
		if (ret < 0) {
			LOGE("compute string failed, out of memory\n");
			free(des);
			goto free_key;
		}

		ret = do_copy_tail(src, des, 0);
		if (ret < 0) {
			LOGE("copy (%s) to (%s) failed, error (%s)\n",
			     src, des, strerror(-ret));
		}

		free(src);
		free(des);
	}

	hist_raise_event("CRASH", crash->name, e->dir, "", key);

free_key:
	free(key);
free_data:
	free(data0);
	free(data1);
	free(data2);
free_trfile:
	free(trfile);
}

static void crashlog_send_info(struct event_t *e)
{
	int id;
	struct info_t *info = (struct info_t *)e->private;
	struct log_t *log;
	char *key = generate_event_id("INFO", info->name);

	if (key == NULL) {
		LOGE("generate event id failed, error (%s)\n",
		     strerror(errno));
		return;
	}

	if (to_collect_logs(info)) {
		e->dir = generate_log_dir(MODE_STATS, key);
		if (e->dir == NULL) {
			LOGE("generate crashlog dir failed\n");
			goto free_key;
		}

		for_each_log_collect(id, log, info) {
			if (!log)
				continue;
			log->get(log, (void *)e->dir);
		}
	}

	hist_raise_event("INFO", info->name, e->dir, "", key);

free_key:
	free(key);
}

static void crashlog_send_uptime(void)
{
	hist_raise_uptime(NULL);
}

static void crashlog_send_reboot(void)
{
	char reason[MAXLINESIZE];
	char *key;

	if (swupdated(get_sender_by_name("crashlog"))) {
		key = generate_event_id("INFO", "SWUPDATE");
		if (key == NULL) {
			LOGE("generate event id failed, error (%s)\n",
			     strerror(errno));
			return;
		}

		hist_raise_event("INFO", "SWUPDATE", NULL, "", key);
		free(key);
	}

	read_startupreason(reason);
	key = generate_event_id("REBOOT", reason);
	if (key == NULL) {
		LOGE("generate event id failed, error (%s)\n",
		     strerror(errno));
		return;
	}

	hist_raise_event("REBOOT", reason, NULL, "", key);
	free(key);
}

static void crashlog_new_vmevent(char *line_to_sync, struct vm_t *vm)
{
	struct sender_t *crashlog;
	char event[96] = {0};
	char longtime[96] = {0};
	char type[96] = {0};
	char rest[PATH_MAX] = {0};
	char vmkey[SHA_DIGEST_LENGTH + 1] = {0};
	char *vmlogpath = NULL;
	char *key;
	char *log;
	char *cmd;
	int ret;
	int quota;
	char *dir;

	/* VM events in history_event like this:
	 *
	 * "CRASH   xxxxxxxxxxxxxxxxxxxx  2017-11-11/03:12:59  JAVACRASH
	 * /data/logs/crashlog0_xxxxxxxxxxxxxxxxxxxx"
	 * "REBOOT  xxxxxxxxxxxxxxxxxxxx  2011-11-11/11:20:51  POWER-ON
	 * 0000:00:00"
	 */
	char *vm_format = "%[^ ]%*[ ]%[^ ]%*[ ]%[^ ]%*[ ]%[^ ]%*[ ]%[^\n]%*c";

	ret = sscanf(line_to_sync, vm_format, event, vmkey, longtime,
		     type, rest);
	if (ret != 5) {
		LOGE("get a invalied line from (%s), skip\n", vm->name);
		return;
	}

	crashlog = get_sender_by_name("crashlog");
	if (!crashlog)
		return;

	quota = atoi(crashlog->spacequota);
	if (!space_available(crashlog->outdir, quota)) {
		hist_raise_infoerror("SPACE_FULL");
		return;
	}

	key = generate_event_id("SOS", vmkey);
	if (key == NULL) {
		LOGE("generate event id failed, error (%s)\n",
		     strerror(errno));
		return;
	}

	dir = generate_log_dir(MODE_VMEVENT, key);
	if (dir == NULL) {
		LOGE("generate crashlog dir failed\n");
		goto free_key;
	}

	/* if line contains log, we need dump each file in the logdir
	 */
	log = strstr(rest, "/logs/");
	if (log) {
		ret = asprintf(&vmlogpath, "%s", log + 1);
		if (ret < 0) {
			LOGE("compute string failed, out of memory\n");
			goto free_dir;
		}

		ret = asprintf(&cmd, "rdump %s %s", vmlogpath, dir);
		if (ret < 0) {
			LOGE("compute string failed, out of memory\n");
			free(vmlogpath);
			goto free_dir;
		}

		debugfs_cmd(loop_dev, cmd, NULL);

		free(cmd);
		free(vmlogpath);
	}

	generate_crashfile(dir, event, key, type, vm->name,
			   vmkey, NULL);
	hist_raise_event(vm->name, type, dir, "", key);

free_dir:
	free(dir);
free_key:
	free(key);
}

static void crashlog_send(struct event_t *e)
{

	int id;
	struct log_t *log;

	for_each_log(id, log, conf) {
		if (!log)
			continue;

		log->get = crashlog_get_log;
	}
	switch (e->event_type) {
	case CRASH:
		crashlog_send_crash(e);
		break;
	case INFO:
		crashlog_send_info(e);
		break;
	case UPTIME:
		crashlog_send_uptime();
		break;
	case REBOOT:
		crashlog_send_reboot();
		break;
	case VM:
		refresh_vm_history(get_sender_by_name("crashlog"),
				   crashlog_new_vmevent);
		break;
	default:
		LOGE("unsupoorted event type %d\n", e->event_type);
	}
}

int init_sender(void)
{
	int ret;
	int id;
	int fd;
	struct sender_t *sender;
	struct uptime_t *uptime;

	for_each_sender(id, sender, conf) {
		if (!sender)
			continue;

		ret = asprintf(&sender->log_vmrecordid, "%s/vmrecordid",
			       sender->outdir);
		if (ret < 0) {
			LOGE("compute string failed, out of memory\n");
			return -ENOMEM;
		}

		if (!directory_exists(sender->outdir))
			if (mkdir_p(sender->outdir) < 0) {
				LOGE("mkdir (%s) failed, error (%s)\n",
				     sender->outdir, strerror(errno));
				return -errno;
			}

		ret = init_properties(sender);
		if (ret) {
			LOGE("init sender failed\n");
			exit(-1);
		}

		/* touch uptime file, to add inotify */
		uptime = sender->uptime;
		if (uptime) {
			fd = open(uptime->path, O_RDWR | O_CREAT, 0666);
			if (fd < 0) {
				LOGE("open failed with (%s, %d), error (%s)\n",
				     uptime->path, atoi(uptime->frequency),
				     strerror(errno));
				return -errno;
			}
			close(fd);
		}

		if (!strncmp(sender->name, "crashlog",
			     strlen(sender->name))) {
			sender->send = crashlog_send;
			ret = prepare_history();
			if (ret)
				return -1;
		}
	}

	return 0;
}
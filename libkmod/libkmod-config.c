// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Copyright (C) 2011-2013  ProFUSION embedded systems
 * Copyright (C) 2013  Intel Corporation. All rights reserved.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <shared/util.h>

#include "libkmod.h"
#include "libkmod-internal.h"

struct kmod_alias {
	char *name;
	char modname[];
};

struct kmod_options {
	char *options;
	char modname[];
};

struct kmod_command {
	char *command;
	char modname[];
};

struct kmod_softdep {
	char *name;
	const char **pre;
	const char **post;
	unsigned int n_pre;
	unsigned int n_post;
};

struct kmod_weakdep {
	char *name;
	const char **weak;
	unsigned int n_weak;
};

const char *kmod_blacklist_get_modname(const struct kmod_list *l)
{
	return l->data;
}

const char *kmod_alias_get_name(const struct kmod_list *l)
{
	const struct kmod_alias *alias = l->data;
	return alias->name;
}

const char *kmod_alias_get_modname(const struct kmod_list *l)
{
	const struct kmod_alias *alias = l->data;
	return alias->modname;
}

const char *kmod_option_get_options(const struct kmod_list *l)
{
	const struct kmod_options *alias = l->data;
	return alias->options;
}

const char *kmod_option_get_modname(const struct kmod_list *l)
{
	const struct kmod_options *alias = l->data;
	return alias->modname;
}

const char *kmod_command_get_command(const struct kmod_list *l)
{
	const struct kmod_command *alias = l->data;
	return alias->command;
}

const char *kmod_command_get_modname(const struct kmod_list *l)
{
	const struct kmod_command *alias = l->data;
	return alias->modname;
}

const char *kmod_softdep_get_name(const struct kmod_list *l)
{
	const struct kmod_softdep *dep = l->data;
	return dep->name;
}

const char *const *kmod_softdep_get_pre(const struct kmod_list *l, unsigned int *count)
{
	const struct kmod_softdep *dep = l->data;
	*count = dep->n_pre;
	return dep->pre;
}

const char *const *kmod_softdep_get_post(const struct kmod_list *l, unsigned int *count)
{
	const struct kmod_softdep *dep = l->data;
	*count = dep->n_post;
	return dep->post;
}

const char *kmod_weakdep_get_name(const struct kmod_list *l)
{
	const struct kmod_weakdep *dep = l->data;
	return dep->name;
}

const char *const *kmod_weakdep_get_weak(const struct kmod_list *l, unsigned int *count)
{
	const struct kmod_weakdep *dep = l->data;
	*count = dep->n_weak;
	return dep->weak;
}
static int kmod_config_add_command(struct kmod_config *config, const char *modname,
				   const char *command, const char *command_name,
				   struct kmod_list **list)
{
	_cleanup_free_ struct kmod_command *cmd;
	struct kmod_list *l;
	size_t modnamelen = strlen(modname) + 1;
	size_t commandlen = strlen(command) + 1;

	DBG(config->ctx, "modname='%s' cmd='%s %s'\n", modname, command_name, command);

	cmd = malloc(sizeof(*cmd) + modnamelen + commandlen);
	if (!cmd)
		return -ENOMEM;

	cmd->command = sizeof(*cmd) + modnamelen + (char *)cmd;
	memcpy(cmd->modname, modname, modnamelen);
	memcpy(cmd->command, command, commandlen);

	l = kmod_list_append(*list, cmd);
	if (!l)
		return -ENOMEM;

	TAKE_PTR(cmd);
	*list = l;

	return 0;
}

static int kmod_config_add_options(struct kmod_config *config, const char *modname,
				   const char *options)
{
	_cleanup_free_ struct kmod_options *opt;
	struct kmod_list *list;
	size_t modnamelen = strlen(modname) + 1;
	size_t optionslen = strlen(options) + 1;

	DBG(config->ctx, "modname='%s' options='%s'\n", modname, options);

	opt = malloc(sizeof(*opt) + modnamelen + optionslen);
	if (!opt)
		return -ENOMEM;

	opt->options = sizeof(*opt) + modnamelen + (char *)opt;

	memcpy(opt->modname, modname, modnamelen);
	memcpy(opt->options, options, optionslen);
	strchr_replace(opt->options, '\t', ' ');

	list = kmod_list_append(config->options, opt);
	if (!list)
		return -ENOMEM;

	TAKE_PTR(opt);
	config->options = list;

	return 0;
}

static int kmod_config_add_alias(struct kmod_config *config, const char *name,
				 const char *modname)
{
	_cleanup_free_ struct kmod_alias *alias;
	struct kmod_list *list;
	size_t namelen = strlen(name) + 1, modnamelen = strlen(modname) + 1;

	DBG(config->ctx, "name=%s modname=%s\n", name, modname);

	alias = malloc(sizeof(*alias) + namelen + modnamelen);
	if (!alias)
		return -ENOMEM;

	alias->name = sizeof(*alias) + modnamelen + (char *)alias;

	memcpy(alias->modname, modname, modnamelen);
	memcpy(alias->name, name, namelen);

	list = kmod_list_append(config->aliases, alias);
	if (!list)
		return -ENOMEM;

	TAKE_PTR(alias);
	config->aliases = list;

	return 0;
}

static int kmod_config_add_blacklist(struct kmod_config *config, const char *modname)
{
	_cleanup_free_ char *p;
	struct kmod_list *list;

	DBG(config->ctx, "modname=%s\n", modname);

	_clang_suppress_alloc_ p = strdup(modname);
	if (!p)
		return -ENOMEM;

	list = kmod_list_append(config->blacklists, p);
	if (!list)
		return -ENOMEM;

	TAKE_PTR(p);
	config->blacklists = list;

	return 0;
}

static int kmod_config_add_softdep(struct kmod_config *config, const char *modname,
				   const char *line)
{
	struct kmod_list *list;
	struct kmod_softdep *dep;
	const char *s, *p;
	char *itr;
	unsigned int n_pre = 0, n_post = 0;
	size_t modnamelen = strlen(modname) + 1;
	size_t buflen = 0, size, size_pre, size_post;
	bool was_space = false;
	enum { S_NONE, S_PRE, S_POST } mode = S_NONE;

	DBG(config->ctx, "modname=%s\n", modname);

	/* analyze and count */
	for (p = s = line;; s++) {
		size_t plen;

		if (*s != '\0') {
			if (!isspace(*s)) {
				was_space = false;
				continue;
			}

			if (was_space) {
				p = s + 1;
				continue;
			}
			was_space = true;

			if (p >= s)
				continue;
		}
		plen = s - p;

		if (plen == strlen("pre:") && strstartswith(p, "pre:"))
			mode = S_PRE;
		else if (plen == strlen("post:") && strstartswith(p, "post:"))
			mode = S_POST;
		else if (*s != '\0' || (*s == '\0' && !was_space)) {
			if (mode == S_PRE) {
				buflen += plen + 1;
				if (uadd32_overflow(n_pre, 1, &n_pre)) {
					ERR(config->ctx,
					    "too many pre softdeps for modname=%s\n",
					    modname);
					return -EINVAL;
				}
			} else if (mode == S_POST) {
				buflen += plen + 1;
				if (uadd32_overflow(n_post, 1, &n_post)) {
					ERR(config->ctx,
					    "too many post softdeps for modname=%s\n",
					    modname);
					return -EINVAL;
				}
			}
		}
		p = s + 1;
		if (*s == '\0')
			break;
	}

	DBG(config->ctx, "%u pre, %u post\n", n_pre, n_post);

	/*
	 * sizeof(struct kmod_softdep) + modnamelen +
	 * n_pre * sizeof(const char *) + n_post * sizeof(const char *) + buflen
	 */
	if (uaddsz_overflow(sizeof(struct kmod_softdep), modnamelen, &size) ||
	    umulsz_overflow(n_pre, sizeof(const char *), &size_pre) ||
	    uaddsz_overflow(size, size_pre, &size) ||
	    umulsz_overflow(n_post, sizeof(const char *), &size_post) ||
	    uaddsz_overflow(size, size_post, &size) ||
	    uaddsz_overflow(size, buflen, &size)) {
		ERR(config->ctx, "out-of-memory modname=%s\n", modname);
		return -ENOMEM;
	}

	dep = malloc(size);
	if (dep == NULL) {
		ERR(config->ctx, "out-of-memory modname=%s\n", modname);
		return -ENOMEM;
	}
	dep->n_pre = n_pre;
	dep->n_post = n_post;
	dep->pre = (const char **)((char *)dep + sizeof(struct kmod_softdep));
	dep->post = dep->pre + n_pre;
	dep->name = (char *)(dep->post + n_post);

	memcpy(dep->name, modname, modnamelen);

	/* copy strings */
	itr = dep->name + modnamelen;
	n_pre = 0;
	n_post = 0;
	mode = S_NONE;
	was_space = false;
	for (p = s = line;; s++) {
		size_t plen;

		if (*s != '\0') {
			if (!isspace(*s)) {
				was_space = false;
				continue;
			}

			if (was_space) {
				p = s + 1;
				continue;
			}
			was_space = true;

			if (p >= s)
				continue;
		}
		plen = s - p;

		if (plen == strlen("pre:") && strstartswith(p, "pre:"))
			mode = S_PRE;
		else if (plen == strlen("post:") && strstartswith(p, "post:"))
			mode = S_POST;
		else if (*s != '\0' || (*s == '\0' && !was_space)) {
			if (mode == S_PRE) {
				dep->pre[n_pre] = itr;
				memcpy(itr, p, plen);
				itr[plen] = '\0';
				itr += plen + 1;
				n_pre++;
			} else if (mode == S_POST) {
				dep->post[n_post] = itr;
				memcpy(itr, p, plen);
				itr[plen] = '\0';
				itr += plen + 1;
				n_post++;
			}
		}
		p = s + 1;
		if (*s == '\0')
			break;
	}

	list = kmod_list_append(config->softdeps, dep);
	if (list == NULL) {
		free(dep);
		return -ENOMEM;
	}
	config->softdeps = list;

	return 0;
}

static int kmod_config_add_weakdep(struct kmod_config *config, const char *modname,
				   const char *line)
{
	struct kmod_list *list;
	struct kmod_weakdep *dep;
	const char *s, *p;
	char *itr;
	unsigned int n_weak = 0;
	size_t modnamelen = strlen(modname) + 1;
	size_t buflen = 0, size, size_weak;
	bool was_space = false;

	DBG(config->ctx, "modname=%s\n", modname);

	/* analyze and count */
	for (p = s = line;; s++) {
		size_t plen;

		if (*s != '\0') {
			if (!isspace(*s)) {
				was_space = false;
				continue;
			}

			if (was_space) {
				p = s + 1;
				continue;
			}
			was_space = true;

			if (p >= s)
				continue;
		}
		plen = s - p;

		if (*s != '\0' || (*s == '\0' && !was_space)) {
			buflen += plen + 1;
			if (uadd32_overflow(n_weak, 1, &n_weak)) {
				ERR(config->ctx, "too many weakdeps for modname=%s\n",
				    modname);
				return -EINVAL;
			}
		}
		p = s + 1;
		if (*s == '\0')
			break;
	}

	DBG(config->ctx, "%u weak\n", n_weak);

	/* sizeof(struct kmod_weakdep) + modnamelen + n_weak * sizeof(const char *) + buflen */
	if (uaddsz_overflow(sizeof(struct kmod_weakdep), modnamelen, &size) ||
	    umulsz_overflow(n_weak, sizeof(const char *), &size_weak) ||
	    uaddsz_overflow(size, size_weak, &size) ||
	    uaddsz_overflow(size, buflen, &size)) {
		ERR(config->ctx, "out-of-memory modname=%s\n", modname);
		return -ENOMEM;
	}

	dep = malloc(size);
	if (dep == NULL) {
		ERR(config->ctx, "out-of-memory modname=%s\n", modname);
		return -ENOMEM;
	}
	dep->n_weak = n_weak;
	dep->weak = (const char **)((char *)dep + sizeof(struct kmod_weakdep));
	dep->name = (char *)(dep->weak + n_weak);

	memcpy(dep->name, modname, modnamelen);

	/* copy strings */
	itr = dep->name + modnamelen;
	n_weak = 0;
	was_space = false;
	for (p = s = line;; s++) {
		size_t plen;

		if (*s != '\0') {
			if (!isspace(*s)) {
				was_space = false;
				continue;
			}

			if (was_space) {
				p = s + 1;
				continue;
			}
			was_space = true;

			if (p >= s)
				continue;
		}
		plen = s - p;

		if (*s != '\0' || (*s == '\0' && !was_space)) {
			dep->weak[n_weak] = itr;
			memcpy(itr, p, plen);
			itr[plen] = '\0';
			itr += plen + 1;
			n_weak++;
		}
		p = s + 1;
		if (*s == '\0')
			break;
	}

	list = kmod_list_append(config->weakdeps, dep);
	if (list == NULL) {
		free(dep);
		return -ENOMEM;
	}
	config->weakdeps = list;

	return 0;
}

static char *softdep_to_char(struct kmod_softdep *dep)
{
	size_t sz = 1; /* at least '\0' */
	size_t sz_pre, sz_post;
	const char *start, *end;
	char *s, *itr;

	/*
	 * Rely on the fact that dep->pre[] and dep->post[] are strv's that
	 * point to a contiguous buffer
	 */
	if (dep->n_pre > 0) {
		start = dep->pre[0];
		end = dep->pre[dep->n_pre - 1] + strlen(dep->pre[dep->n_pre - 1]);
		sz_pre = end - start;
		sz += sz_pre + strlen("pre: ");
	} else
		sz_pre = 0;

	if (dep->n_post > 0) {
		start = dep->post[0];
		end = dep->post[dep->n_post - 1] + strlen(dep->post[dep->n_post - 1]);
		sz_post = end - start;
		sz += sz_post + strlen("post: ");
	} else
		sz_post = 0;

	itr = s = malloc(sz);
	if (s == NULL)
		return NULL;

	if (sz_pre) {
		char *p;

		memcpy(itr, "pre: ", strlen("pre: "));
		itr += strlen("pre: ");

		/* include last '\0' */
		memcpy(itr, dep->pre[0], sz_pre + 1);
		for (p = itr; p < itr + sz_pre; p++) {
			if (*p == '\0')
				*p = ' ';
		}
		itr = p;
	}

	if (sz_post) {
		char *p;

		memcpy(itr, "post: ", strlen("post: "));
		itr += strlen("post: ");

		/* include last '\0' */
		memcpy(itr, dep->post[0], sz_post + 1);
		for (p = itr; p < itr + sz_post; p++) {
			if (*p == '\0')
				*p = ' ';
		}
		itr = p;
	}

	*itr = '\0';

	return s;
}

static char *weakdep_to_char(struct kmod_weakdep *dep)
{
	size_t sz = 1; /* at least '\0' */
	size_t sz_weak;
	const char *start, *end;
	char *s, *itr;

	/* Rely on the fact that dep->weak[] is a strv that points to a contiguous buffer */
	if (dep->n_weak > 0) {
		start = dep->weak[0];
		end = dep->weak[dep->n_weak - 1] + strlen(dep->weak[dep->n_weak - 1]);
		sz_weak = end - start;
		sz += sz_weak;
	} else
		sz_weak = 0;

	itr = s = malloc(sz);
	if (s == NULL)
		return NULL;

	if (sz_weak) {
		char *p;

		/* include last '\0' */
		memcpy(itr, dep->weak[0], sz_weak);
		for (p = itr; p < itr + sz_weak; p++) {
			if (*p == '\0')
				*p = ' ';
		}
		itr = p;
	}

	*itr = '\0';

	return s;
}

static void kcmdline_parse_result(struct kmod_config *config, char *modname, char *param,
				  char *value)
{
	if (modname == NULL || param == NULL)
		return;

	DBG(config->ctx, "%s %s\n", modname, param);

	if (streq(modname, "modprobe") && strstartswith(param, "blacklist=")) {
		for (;;) {
			char *t = strsep(&value, ",");
			if (t == NULL)
				break;

			kmod_config_add_blacklist(config, t);
		}
	} else {
		if (underscores(modname) < 0) {
			ERR(config->ctx,
			    "Ignoring bad option on kernel command line while parsing module name: '%s'\n",
			    modname);
		} else {
			kmod_config_add_options(config, modname, param);
		}
	}
}

static int kmod_config_parse_kcmdline(struct kmod_config *config)
{
	char buf[KCMD_LINE_SIZE];
	int fd, err;
	char *p, *p_quote_start, *modname, *param = NULL, *value = NULL;
	bool is_quoted = false, iter = true;
	enum state {
		STATE_IGNORE,
		STATE_MODNAME,
		STATE_PARAM,
		STATE_VALUE,
		STATE_COMPLETE,
	} state;

	fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		err = -errno;
		DBG(config->ctx, "could not open '/proc/cmdline' for reading: %m\n");
		return err;
	}

	err = read_str_safe(fd, buf, sizeof(buf));
	close(fd);
	if (err < 0) {
		ERR(config->ctx, "could not read from '/proc/cmdline': %s\n",
		    strerror(-err));
		return err;
	}

	state = STATE_MODNAME;
	p_quote_start = NULL;
	for (p = buf, modname = buf; iter; p++) {
		switch (*p) {
		case '"':
			is_quoted = !is_quoted;

			/*
			 * only allow starting quote as first char when looking
			 * for a modname: anything else is considered ill-formed
			 */
			if (is_quoted && state == STATE_MODNAME && p == modname) {
				p_quote_start = p;
				modname = p + 1;
			} else if (state != STATE_VALUE) {
				state = STATE_IGNORE;
			}

			break;
		case '\0':
			iter = false;
			/* fall-through */
		case ' ':
		case '\n':
		case '\t':
		case '\v':
		case '\f':
		case '\r':
			if (is_quoted && state == STATE_VALUE) {
				/* no state change*/;
			} else if (is_quoted) {
				/* spaces are only allowed in the value part */
				state = STATE_IGNORE;
			} else if (state == STATE_VALUE || state == STATE_PARAM) {
				*p = '\0';
				state = STATE_COMPLETE;
			} else {
				/*
				 * go to next option, ignoring any possible
				 * partial match we have
				 */
				modname = p + 1;
				state = STATE_MODNAME;
				p_quote_start = NULL;
			}
			break;
		case '.':
			if (state == STATE_MODNAME) {
				*p = '\0';
				param = p + 1;
				state = STATE_PARAM;
			} else if (state == STATE_PARAM) {
				state = STATE_IGNORE;
			}
			break;
		case '=':
			if (state == STATE_PARAM) {
				/*
				 * Don't set *p to '\0': the value var shadows
				 * param
				 */
				value = p + 1;
				state = STATE_VALUE;
			} else if (state == STATE_MODNAME) {
				state = STATE_IGNORE;
			}
			break;
		}

		if (state == STATE_COMPLETE) {
			/*
			 * We may need to re-quote to unmangle what the
			 * bootloader passed. Example: grub passes the option as
			 * "parport.dyndbg=file drivers/parport/ieee1284_ops.c +mpf"
			 * instead of
			 * parport.dyndbg="file drivers/parport/ieee1284_ops.c +mpf"
			 */
			if (p_quote_start && p_quote_start < modname) {
				/*
				 * p_quote_start
				 * |
				 * |modname  param  value
				 * ||        |      |
				 * vv        v      v
				 * "parport\0dyndbg=file drivers/parport/ieee1284_ops.c +mpf" */
				memmove(p_quote_start, modname, value - modname);
				value--;
				modname--;
				param--;
				*value = '"';
			}
			kcmdline_parse_result(config, modname, param, value);
			/* start over on next iteration */
			modname = p + 1;
			state = STATE_MODNAME;
			p_quote_start = NULL;
		}
	}

	return 0;
}

/*
 * Take an fd and own it. It will be closed on return. filename is used only
 * for debug messages
 */
static int kmod_config_parse(struct kmod_config *config, int fd, const char *filename)
{
	struct kmod_ctx *ctx = config->ctx;
	char *line;
	FILE *fp;
	unsigned int linenum = 0;
	int err;

	fp = fdopen(fd, "r");
	if (fp == NULL) {
		err = -errno;
		ERR(config->ctx, "fd %d: %m\n", fd);
		close(fd);
		return err;
	}

	while ((line = freadline_wrapped(fp, &linenum)) != NULL) {
		char *cmd, *saveptr;

		if (line[0] == '\0' || line[0] == '#')
			goto done_next;

		cmd = strtok_r(line, "\t ", &saveptr);
		if (cmd == NULL)
			goto done_next;

		if (streq(cmd, "alias")) {
			char *alias = strtok_r(NULL, "\t ", &saveptr);
			char *modname = strtok_r(NULL, "\t ", &saveptr);

			if (underscores(alias) < 0 || underscores(modname) < 0)
				goto syntax_error;

			kmod_config_add_alias(config, alias, modname);
		} else if (streq(cmd, "blacklist")) {
			char *modname = strtok_r(NULL, "\t ", &saveptr);

			if (underscores(modname) < 0)
				goto syntax_error;

			kmod_config_add_blacklist(config, modname);
		} else if (streq(cmd, "options")) {
			char *modname = strtok_r(NULL, "\t ", &saveptr);
			char *options = strtok_r(NULL, "\0", &saveptr);

			if (underscores(modname) < 0 || options == NULL)
				goto syntax_error;

			kmod_config_add_options(config, modname, options);
		} else if (streq(cmd, "install")) {
			char *modname = strtok_r(NULL, "\t ", &saveptr);
			char *installcmd = strtok_r(NULL, "\0", &saveptr);

			if (underscores(modname) < 0 || installcmd == NULL)
				goto syntax_error;

			kmod_config_add_command(config, modname, installcmd, cmd,
						&config->install_commands);
		} else if (streq(cmd, "remove")) {
			char *modname = strtok_r(NULL, "\t ", &saveptr);
			char *removecmd = strtok_r(NULL, "\0", &saveptr);

			if (underscores(modname) < 0 || removecmd == NULL)
				goto syntax_error;

			kmod_config_add_command(config, modname, removecmd, cmd,
						&config->remove_commands);
		} else if (streq(cmd, "softdep")) {
			char *modname = strtok_r(NULL, "\t ", &saveptr);
			char *softdeps = strtok_r(NULL, "\0", &saveptr);

			if (underscores(modname) < 0 || softdeps == NULL)
				goto syntax_error;

			kmod_config_add_softdep(config, modname, softdeps);
		} else if (streq(cmd, "weakdep")) {
			char *modname = strtok_r(NULL, "\t ", &saveptr);
			char *weakdeps = strtok_r(NULL, "\0", &saveptr);

			if (underscores(modname) < 0 || weakdeps == NULL)
				goto syntax_error;

			kmod_config_add_weakdep(config, modname, weakdeps);
		} else if (streq(cmd, "include") || streq(cmd, "config")) {
			ERR(ctx, "%s: command %s is deprecated and not parsed anymore\n",
			    filename, cmd);
		} else {
syntax_error:
			ERR(ctx, "%s line %u: ignoring bad line starting with '%s'\n",
			    filename, linenum, cmd);
		}

done_next:
		free(line);
	}

	fclose(fp);

	return 0;
}

void kmod_config_free(struct kmod_config *config)
{
	kmod_list_release(config->aliases, free);
	kmod_list_release(config->blacklists, free);
	kmod_list_release(config->options, free);
	kmod_list_release(config->install_commands, free);
	kmod_list_release(config->remove_commands, free);
	kmod_list_release(config->softdeps, free);
	kmod_list_release(config->weakdeps, free);
	kmod_list_release(config->paths, free);
	free(config);
}

static bool conf_files_filter_out(struct kmod_ctx *ctx, DIR *d, const char *path,
				  const char *fn)
{
	size_t len = strlen(fn);
	struct stat st;

	if (fn[0] == '.')
		return true;

	if (len < 6 || !streq(&fn[len - 5], ".conf"))
		return true;

	if (fstatat(dirfd(d), fn, &st, 0) < 0) {
		ERR(ctx, "Cannot stat directory entry: %s/%s\n", path, fn);
		return true;
	}

	if (S_ISDIR(st.st_mode)) {
		ERR(ctx,
		    "Directories inside directories are not supported: "
		    "%s/%s\n",
		    path, fn);
		return true;
	}

	return false;
}

struct conf_file {
	const char *path;
	bool is_single;
	char name[];
};

static int conf_files_insert_sorted(struct kmod_ctx *ctx, struct kmod_list **list,
				    const char *path, const char *name)
{
	struct kmod_list *lpos, *tmp;
	struct conf_file *cf;
	size_t namelen;
	int cmp = -1;
	bool is_single = false;

	if (name == NULL) {
		name = basename(path);
		is_single = true;
	}

	kmod_list_foreach(lpos, *list) {
		cf = lpos->data;

		if ((cmp = strcmp(name, cf->name)) <= 0)
			break;
	}

	if (cmp == 0) {
		DBG(ctx, "Ignoring duplicate config file: %s/%s\n", path, name);
		return -EEXIST;
	}

	namelen = strlen(name);
	cf = malloc(sizeof(*cf) + namelen + 1);
	if (cf == NULL)
		return -ENOMEM;

	memcpy(cf->name, name, namelen + 1);
	cf->path = path;
	cf->is_single = is_single;

	if (lpos == NULL)
		tmp = kmod_list_append(*list, cf);
	else if (lpos == *list)
		tmp = kmod_list_prepend(*list, cf);
	else
		tmp = kmod_list_insert_before(lpos, cf);

	if (tmp == NULL) {
		free(cf);
		return -ENOMEM;
	}

	if (lpos == NULL || lpos == *list)
		*list = tmp;

	return 0;
}

/*
 * Insert configuration files in @list, ignoring duplicates
 */
static int conf_files_list(struct kmod_ctx *ctx, struct kmod_list **list,
			   const char *path, unsigned long long *path_stamp)
{
	DIR *d;
	int err;
	struct stat st;
	struct dirent *dent;

	if (stat(path, &st) != 0) {
		err = -errno;
		DBG(ctx, "could not stat '%s': %m\n", path);
		return err;
	}

	*path_stamp = stat_mstamp(&st);

	if (!S_ISDIR(st.st_mode)) {
		conf_files_insert_sorted(ctx, list, path, NULL);
		return 0;
	}

	d = opendir(path);
	if (d == NULL) {
		ERR(ctx, "opendir(%s): %m\n", path);
		return -EINVAL;
	}

	for (dent = readdir(d); dent != NULL; dent = readdir(d)) {
		if (conf_files_filter_out(ctx, d, path, dent->d_name))
			continue;

		conf_files_insert_sorted(ctx, list, path, dent->d_name);
	}

	closedir(d);
	return 0;
}

int kmod_config_new(struct kmod_ctx *ctx, struct kmod_config **p_config,
		    const char *const *config_paths)
{
	struct kmod_config *config;
	struct kmod_list *list = NULL;
	struct kmod_list *path_list = NULL;
	size_t i;

	conf_files_insert_sorted(ctx, &list, kmod_get_dirname(ctx), "modules.softdep");
	conf_files_insert_sorted(ctx, &list, kmod_get_dirname(ctx), "modules.weakdep");

	for (i = 0; config_paths[i] != NULL; i++) {
		const char *path = config_paths[i];
		unsigned long long path_stamp = 0;
		size_t pathlen;
		struct kmod_list *tmp;
		struct kmod_config_path *cf;

		if (conf_files_list(ctx, &list, path, &path_stamp) < 0)
			continue;

		pathlen = strlen(path) + 1;
		cf = malloc(sizeof(*cf) + pathlen);
		if (cf == NULL)
			goto oom;

		cf->stamp = path_stamp;
		memcpy(cf->path, path, pathlen);

		tmp = kmod_list_append(path_list, cf);
		if (tmp == NULL) {
			free(cf);
			goto oom;
		}
		path_list = tmp;
	}

	*p_config = config = calloc(1, sizeof(struct kmod_config));
	if (config == NULL)
		goto oom;

	config->paths = path_list;
	config->ctx = ctx;

	for (; list != NULL; list = kmod_list_remove(list)) {
		char buf[PATH_MAX];
		const char *fn = buf;
		struct conf_file *cf = list->data;
		int fd;

		if (cf->is_single) {
			fn = cf->path;
		} else if (snprintf(buf, sizeof(buf), "%s/%s", cf->path, cf->name) >=
			   (int)sizeof(buf)) {
			ERR(ctx, "Error parsing %s/%s: path too long\n", cf->path,
			    cf->name);
			free(cf);
			continue;
		}

		fd = open(fn, O_RDONLY | O_CLOEXEC);
		DBG(ctx, "parsing file '%s' fd=%d\n", fn, fd);

		if (fd >= 0)
			kmod_config_parse(config, fd, fn);

		free(cf);
	}

	kmod_config_parse_kcmdline(config);

	return 0;

oom:
	kmod_list_release(list, free);
	kmod_list_release(path_list, free);

	return -ENOMEM;
}

/**********************************************************************
 * struct kmod_config_iter functions
 **********************************************************************/

enum config_type {
	CONFIG_TYPE_BLACKLIST = 0,
	CONFIG_TYPE_INSTALL,
	CONFIG_TYPE_REMOVE,
	CONFIG_TYPE_ALIAS,
	CONFIG_TYPE_OPTION,
	CONFIG_TYPE_SOFTDEP,
	CONFIG_TYPE_WEAKDEP,
};

struct kmod_config_iter {
	enum config_type type;
	bool intermediate;
	const struct kmod_list *list;
	const struct kmod_list *curr;
	void *data;
	const char *(*get_key)(const struct kmod_list *l);
	const char *(*get_value)(const struct kmod_list *l);
};

static const char *softdep_get_plain_softdep(const struct kmod_list *l)
{
	char *s = softdep_to_char(l->data);
	return s;
}

static const char *weakdep_get_plain_weakdep(const struct kmod_list *l)
{
	char *s = weakdep_to_char(l->data);
	return s;
}

static struct kmod_config_iter *kmod_config_iter_new(const struct kmod_ctx *ctx,
						     enum config_type type)
{
	struct kmod_config_iter *iter = calloc(1, sizeof(*iter));
	const struct kmod_config *config = kmod_get_config(ctx);

	if (iter == NULL)
		return NULL;

	iter->type = type;

	switch (type) {
	case CONFIG_TYPE_BLACKLIST:
		iter->list = config->blacklists;
		iter->get_key = kmod_blacklist_get_modname;
		break;
	case CONFIG_TYPE_INSTALL:
		iter->list = config->install_commands;
		iter->get_key = kmod_command_get_modname;
		iter->get_value = kmod_command_get_command;
		break;
	case CONFIG_TYPE_REMOVE:
		iter->list = config->remove_commands;
		iter->get_key = kmod_command_get_modname;
		iter->get_value = kmod_command_get_command;
		break;
	case CONFIG_TYPE_ALIAS:
		iter->list = config->aliases;
		iter->get_key = kmod_alias_get_name;
		iter->get_value = kmod_alias_get_modname;
		break;
	case CONFIG_TYPE_OPTION:
		iter->list = config->options;
		iter->get_key = kmod_option_get_modname;
		iter->get_value = kmod_option_get_options;
		break;
	case CONFIG_TYPE_SOFTDEP:
		iter->list = config->softdeps;
		iter->get_key = kmod_softdep_get_name;
		iter->get_value = softdep_get_plain_softdep;
		iter->intermediate = true;
		break;
	case CONFIG_TYPE_WEAKDEP:
		iter->list = config->weakdeps;
		iter->get_key = kmod_weakdep_get_name;
		iter->get_value = weakdep_get_plain_weakdep;
		iter->intermediate = true;
		break;
	}

	return iter;
}

KMOD_EXPORT struct kmod_config_iter *kmod_config_get_blacklists(const struct kmod_ctx *ctx)
{
	if (ctx == NULL)
		return NULL;

	return kmod_config_iter_new(ctx, CONFIG_TYPE_BLACKLIST);
}

// clang-format off
KMOD_EXPORT struct kmod_config_iter *kmod_config_get_install_commands(const struct kmod_ctx *ctx)
// clang-format on
{
	if (ctx == NULL)
		return NULL;

	return kmod_config_iter_new(ctx, CONFIG_TYPE_INSTALL);
}

// clang-format off
KMOD_EXPORT struct kmod_config_iter *kmod_config_get_remove_commands(const struct kmod_ctx *ctx)
// clang-format on
{
	if (ctx == NULL)
		return NULL;

	return kmod_config_iter_new(ctx, CONFIG_TYPE_REMOVE);
}

KMOD_EXPORT struct kmod_config_iter *kmod_config_get_aliases(const struct kmod_ctx *ctx)
{
	if (ctx == NULL)
		return NULL;

	return kmod_config_iter_new(ctx, CONFIG_TYPE_ALIAS);
}

KMOD_EXPORT struct kmod_config_iter *kmod_config_get_options(const struct kmod_ctx *ctx)
{
	if (ctx == NULL)
		return NULL;

	return kmod_config_iter_new(ctx, CONFIG_TYPE_OPTION);
}

KMOD_EXPORT struct kmod_config_iter *kmod_config_get_softdeps(const struct kmod_ctx *ctx)
{
	if (ctx == NULL)
		return NULL;

	return kmod_config_iter_new(ctx, CONFIG_TYPE_SOFTDEP);
}

KMOD_EXPORT struct kmod_config_iter *kmod_config_get_weakdeps(const struct kmod_ctx *ctx)
{
	if (ctx == NULL)
		return NULL;

	return kmod_config_iter_new(ctx, CONFIG_TYPE_WEAKDEP);
}

KMOD_EXPORT const char *kmod_config_iter_get_key(const struct kmod_config_iter *iter)
{
	if (iter == NULL || iter->curr == NULL)
		return NULL;

	return iter->get_key(iter->curr);
}

KMOD_EXPORT const char *kmod_config_iter_get_value(const struct kmod_config_iter *iter)
{
	const char *s;

	if (iter == NULL || iter->curr == NULL)
		return NULL;

	if (iter->get_value == NULL)
		return NULL;

	if (iter->intermediate) {
		struct kmod_config_iter *i = (struct kmod_config_iter *)iter;

		free(i->data);
		s = i->data = (void *)iter->get_value(iter->curr);
	} else
		s = iter->get_value(iter->curr);

	return s;
}

KMOD_EXPORT bool kmod_config_iter_next(struct kmod_config_iter *iter)
{
	if (iter == NULL)
		return false;

	if (iter->curr == NULL) {
		iter->curr = iter->list;
		return iter->curr != NULL;
	}

	iter->curr = kmod_list_next(iter->list, iter->curr);

	return iter->curr != NULL;
}

KMOD_EXPORT void kmod_config_iter_free_iter(struct kmod_config_iter *iter)
{
	if (iter)
		free(iter->data);
	free(iter);
}

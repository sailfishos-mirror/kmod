// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Copyright (C) 2012-2013  ProFUSION embedded systems
 * Copyright (C) 2012-2013  Lucas De Marchi <lucas.de.marchi@gmail.com>
 */

#ifndef HAVE_FINIT_MODULE
#define HAVE_FINIT_MODULE 1
#endif

#include <assert.h>
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>

#include <shared/util.h>

/* kmod_elf_get_section() is not exported, we need the private header */
#include <libkmod/libkmod-internal.h>

/* FIXME: hack, change name so we don't clash */
#undef ERR
#include "testsuite.h"
#include "stripped-module.h"

struct mod {
	struct mod *next;
	int ret;
	int errcode;
	char name[];
};

static struct mod *modules;
static bool need_init = true;
static struct kmod_ctx *ctx;

static void parse_retcodes(struct mod **_modules, const char *s)
{
	const char *p;

	if (s == NULL)
		return;

	for (p = s;;) {
		struct mod *mod;
		const char *modname;
		char *end;
		size_t modnamelen;
		int ret, errcode;
		long l;

		modname = p;
		if (modname == NULL || modname[0] == '\0')
			break;

		modnamelen = strcspn(s, ":");
		if (modname[modnamelen] != ':')
			break;

		p = modname + modnamelen + 1;
		if (p == NULL)
			break;

		l = strtol(p, &end, 0);
		if (end == p || *end != ':' || errno == ERANGE || l < INT_MIN ||
		    l > INT_MAX)
			break;
		ret = (int)l;
		p = end + 1;

		errno = 0;
		l = strtol(p, &end, 0);
		if (errno == ERANGE || l < INT_MIN || l > INT_MAX)
			break;
		if (*end == ':')
			p = end + 1;
		else if (*end != '\0')
			break;

		errcode = (int)l;

		mod = malloc(sizeof(*mod) + modnamelen + 1);
		if (mod == NULL)
			break;

		memcpy(mod->name, modname, modnamelen);
		mod->name[modnamelen] = '\0';
		mod->ret = ret;
		mod->errcode = errcode;
		mod->next = *_modules;
		*_modules = mod;
	}
}

static int write_one_line_file(const char *fn, const char *line)
{
	FILE *f;
	int r;

	assert(fn);
	assert(line);

	f = fopen(fn, "we");
	if (!f)
		return -errno;

	errno = 0;
	if (fputs(line, f) < 0) {
		r = -errno;
		goto finish;
	}

	fflush(f);

	if (ferror(f)) {
		if (errno != 0)
			r = -errno;
		else
			r = -EIO;
	} else
		r = 0;

finish:
	fclose(f);
	return r;
}

static int create_sysfs_files(const char *modname)
{
	char buf[PATH_MAX];
	const char *sysfsmod = "/sys/module/";
	int len = strlen(sysfsmod);

	memcpy(buf, sysfsmod, len);
	strcpy(buf + len, modname);
	len += strlen(modname);

	assert(mkdir_p(buf, len, 0755) >= 0);

	strcpy(buf + len, "/initstate");
	return write_one_line_file(buf, "live\n");
}

static struct mod *find_module(struct mod *_modules, const char *modname)
{
	struct mod *mod;

	for (mod = _modules; mod != NULL; mod = mod->next) {
		if (streq(mod->name, modname))
			return mod;
	}

	return NULL;
}

static void init_retcodes(void)
{
	const char *s;

	if (!need_init)
		return;

	need_init = false;
	s = getenv(S_TC_INIT_MODULE_RETCODES);
	if (s == NULL) {
		fprintf(stderr, "TRAP init_module(): missing export %s?\n",
			S_TC_INIT_MODULE_RETCODES);
	}

	ctx = kmod_new(NULL, NULL);

	parse_retcodes(&modules, s);
}

static inline bool module_is_inkernel(const char *modname)
{
	struct kmod_module *mod;
	int state;
	bool ret;

	if (kmod_module_new_from_name(ctx, modname, &mod) < 0)
		return false;

	state = kmod_module_get_initstate(mod);

	if (state == KMOD_MODULE_LIVE || state == KMOD_MODULE_BUILTIN)
		ret = true;
	else
		ret = false;

	kmod_module_unref(mod);

	return ret;
}

static uint8_t elf_identify(void *mem)
{
	uint8_t *p = mem;
	return p[EI_CLASS];
}

TS_EXPORT long init_module(void *mem, unsigned long len, const char *args);

/*
 * Default behavior is to try to mimic init_module behavior inside the kernel.
 * If it is a simple test that you know the error code, set the return code
 * in TESTSUITE_INIT_MODULE_RETCODES env var instead.
 *
 * The exception is when the module name is not find in the memory passed.
 * This is because we want to be able to pass dummy modules (and not real
 * ones) and it still work.
 */
/* TODO: add simple validation of the args passed and remove the _maybe_unused_ workaround */
long init_module(void *mem, unsigned long len, _maybe_unused_ const char *args)
{
	const char *modname;
	struct kmod_elf *elf;
	struct mod *mod;
	const void *buf;
	uint64_t off;
	uint64_t bufsize;
	int err;
	uint8_t class;
	off_t offset;

	init_retcodes();

	if (mem == NULL) {
		errno = EFAULT;
		return -1;
	}

	err = kmod_elf_new(mem, len, &elf);
	if (err < 0) {
		errno = -err;
		return -1;
	}

	err = kmod_elf_get_section(elf, ".gnu.linkonce.this_module", &off, &bufsize);
	buf = (const char *)kmod_elf_get_memory(elf) + off;
	kmod_elf_unref(elf);

	if (err < 0)
		return -1;

	/* We need to open both 32 and 64 bits module - hack! */
	class = elf_identify(mem);
	if (class == ELFCLASS64)
		offset = MODULE_NAME_OFFSET_64;
	else
		offset = MODULE_NAME_OFFSET_32;

	modname = (char *)buf + offset;
	mod = find_module(modules, modname);
	if (mod != NULL) {
		errno = mod->errcode;
		err = mod->ret;
	} else if (module_is_inkernel(modname)) {
		err = -1;
		errno = EEXIST;
	} else
		err = 0;

	if (err == 0)
		create_sysfs_files(modname);

	return err;
}

static int check_kernel_version(int major, int minor)
{
	struct utsname u;
	const char *p;
	int maj = 0, min = 0;

	if (uname(&u) < 0)
		return false;
	for (p = u.release; *p >= '0' && *p <= '9'; p++)
		maj = maj * 10 + *p - '0';
	if (*p == '.')
		for (p++; *p >= '0' && *p <= '9'; p++)
			min = min * 10 + *p - '0';
	if (maj > major || (maj == major && min >= minor))
		return true;
	return false;
}

TS_EXPORT int finit_module(const int fd, const char *args, const int flags);

/* TODO: add simple validation of the flags passed and remove the _maybe_unused_ workaround */
int finit_module(const int fd, const char *args, _maybe_unused_ const int flags)
{
	int err;
	void *mem;
	unsigned long len;
	struct stat st;

	if (!check_kernel_version(3, 8)) {
		errno = ENOSYS;
		return -1;
	}
	if (fstat(fd, &st) < 0)
		return -1;

	len = st.st_size;
	mem = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
	if (mem == MAP_FAILED)
		return -1;

	err = init_module(mem, len, args);
	munmap(mem, len);

	return err;
}

TS_EXPORT long int syscall(long int __sysno, ...)
{
	va_list ap;
	long ret;

	switch (__sysno) {
	case -1:
#ifdef __NR_riscv_hwprobe
	case __NR_riscv_hwprobe:
#endif
		errno = ENOSYS;
		return -1;
	case __NR_finit_module:;
		const char *args;
		int flags;
		int fd;

		va_start(ap, __sysno);

		fd = va_arg(ap, int);
		args = va_arg(ap, const char *);
		flags = va_arg(ap, int);

		ret = finit_module(fd, args, flags);

		va_end(ap);
		return ret;
	case __NR_gettid:;
		static long (*nextlib_syscall)(long number, ...);

		if (nextlib_syscall == NULL) {
			nextlib_syscall = dlsym(RTLD_NEXT, "syscall");
			if (nextlib_syscall == NULL) {
				fprintf(stderr,
					"FIXME FIXME FIXME: could not load syscall symbol: %s\n",
					dlerror());
				abort();
			}
		}

		return nextlib_syscall(__NR_gettid);
	}

	/*
	 * FIXME: no way to call the libc function due since this is a
	 * variadic argument function and we don't have a vsyscall() variant
	 * this may fail if a library or process is trying to call syscall()
	 * directly, for example to implement gettid().
	 */
	fprintf(stderr,
		"FIXME FIXME FIXME: could not wrap call to syscall(%ld), this should not happen\n",
		__sysno);

	abort();
}

/* the test is going away anyway, but lets keep valgrind happy */
void free_resources(void) __attribute__((destructor));
void free_resources(void)
{
	while (modules) {
		struct mod *mod = modules->next;
		free(modules);
		modules = mod;
	}

	if (ctx)
		kmod_unref(ctx);
}

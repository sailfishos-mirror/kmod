## kmod - Linux kernel module handling

[![Coverity Scan Status](https://scan.coverity.com/projects/2096/badge.svg)](https://scan.coverity.com/projects/2096)


Information
===========

Mailing list:
	linux-modules@vger.kernel.org (no subscription needed)
	https://lore.kernel.org/linux-modules/

Signed packages:
	http://www.kernel.org/pub/linux/utils/kernel/kmod/

Git:
	git://git.kernel.org/pub/scm/utils/kernel/kmod/kmod.git
	http://git.kernel.org/pub/scm/utils/kernel/kmod/kmod.git
	https://git.kernel.org/pub/scm/utils/kernel/kmod/kmod.git

Gitweb:
	http://git.kernel.org/?p=utils/kernel/kmod/kmod.git
	https://github.com/kmod-project/kmod

Irc:
	#kmod on irc.freenode.org

License:
	LGPLv2.1+ for libkmod, testsuite and helper libraries
	GPLv2+ for tools/*


OVERVIEW
========

kmod is a set of tools to handle common tasks with Linux kernel modules like
insert, remove, list, check properties, resolve dependencies and aliases.

These tools are designed on top of libkmod, a library that is shipped with
kmod. See libkmod/README for more details on this library and how to use it.
The aim is to be compatible with tools, configurations and indexes from
module-init-tools project.

Compilation and installation
============================

In order to compiler the source code you need following software packages:
	- GCC compiler
	- GNU C library

Optional dependencies:
	- ZLIB library
	- LZMA library
	- ZSTD library
	- OPENSSL library (signature handling in modinfo)

Typical configuration:
	./configure CFLAGS="-g -O2" --prefix=/usr \
			--sysconfdir=/etc --libdir=/usr/lib

Configure automatically searches for all required components and packages.

To compile and install run:
	make && make install

Hacking
=======

Run 'autogen.sh' script before configure. If you want to accept the recommended
flags, you just need to run 'autogen.sh c'.

Make sure to read the CODING-STYLE file and the other READMEs: libkmod/README
and testsuite/README.

Compatibility with module-init-tools
====================================

kmod replaces module-init-tools, which is end-of-life. Most of its tools are
rewritten on top of libkmod so it can be used as a drop in replacements.
Somethings however were changed. Reasons vary from "the feature was already
long deprecated on module-init-tools" to "it would be too much trouble to
support it".

There are several features that are being added in kmod, but we don't
keep track of them here.

modprobe
--------

* 'modprobe -l' was marked as deprecated and does not exist anymore

* 'modprobe -t' is gone, together with 'modprobe -l'

* modprobe doesn't parse configuration files with names not ending in
  '.alias' or '.conf'. modprobe used to warn about these files.

* modprobe doesn't parse 'config' and 'include' commands in configuration
  files.

* modprobe from m-i-t does not honour softdeps for install commands. E.g.:
  config:

        install bli "echo bli"
	install bla "echo bla"
	softdep bla pre: bli

  With m-i-t, the output of 'modprobe --show-depends bla' will be:
        install "echo bla"

  While with kmod:
        install "echo bli"
        install "echo bla"

* kmod doesn't dump the configuration as is in the config files. Instead it
  dumps the configuration as it was parsed. Therefore, comments and file names
  are not dumped, but on the good side we know what the exact configuration
  kmod is using. We did this because if we only want to know the entire content
  of configuration files, it's enough to use find(1) in modprobe.d directories

depmod
------

* there's no 'depmod -m' option: legacy modules.*map files are gone

lsmod
-----

* module-init-tools used /proc/modules to parse module info. kmod uses
  /sys/module/*, but there's a fallback to /proc/modules if the latter isn't
  available

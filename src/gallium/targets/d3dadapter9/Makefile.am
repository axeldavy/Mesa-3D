# Copyright © 2012 Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
# HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
# WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

include $(top_srcdir)/src/gallium/Automake.inc

AM_CFLAGS = \
	$(NINE_CFLAGS) \
	$(GALLIUM_CFLAGS) \
	$(VISIBILITY_CFLAGS) \
	-I$(top_srcdir)/src/gallium/winsys \
	-I$(top_srcdir)/src/gallium/state_trackers/nine \
	-I$(top_srcdir)/include/D3D9 \
	-DD3DADAPTER9_DEFAULT_SEARCH_PATH=\"$(NINE_DRIVER_INSTALL_DIR)\" \
	$(GALLIUM_PIPE_LOADER_DEFINES)

lib_LTLIBRARIES = libd3dadapter9.la

libd3dadapter9_la_SOURCES = getproc.c
libd3dadapter9_la_LDFLAGS = -no-undefined -shared
libd3dadapter9_la_LIBADD = \
	$(top_builddir)/src/gallium/auxiliary/libgallium.la \
	$(top_builddir)/src/gallium/auxiliary/pipe-loader/libpipe_loader.la \
	$(top_builddir)/src/gallium/state_trackers/nine/libninetracker.la \
	$(top_builddir)/src/gallium/winsys/sw/wrapper/libwsw.la \
	$(top_builddir)/src/gallium/winsys/sw/null/libws_null.la \
	$(GALLIUM_PIPE_LOADER_LIBS) \
	$(LIBUDEV_LIBS) \
	$(NINE_LIBS)

if HAVE_MESA_LLVM
libd3dadapter9_la_SOURCES += dummy.cpp
libd3dadapter9_la_LDFLAGS += $(LLVM_LDFLAGS)
libd3dadapter9_la_LIBADD += $(LLVM_LIBS)
endif

if HAVE_DRM_LOADER_GALLIUM
libd3dadapter9_la_SOURCES += drm.c
endif

# Provide compatibility with scripts for the old Mesa build system for
# a while by putting a link to the driver into /lib of the build tree.
all-local: libd3dadapter9.la
	$(MKDIR_P) $(top_builddir)/$(LIB_DIR)/gallium
	ln -f .libs/libd3dadapter9.so* $(top_builddir)/$(LIB_DIR)/gallium/
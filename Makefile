# contrib/monetdb_fdw/Makefile

MODULE_big = monetdb_fdw
OBJS = \
	$(WIN32RES) \
	connection.o \
	deparse.o \
	option.o \
	monetdb_fdw.o \
	shippable.o
PGFILEDESC = "monetdb_fdw - foreign data wrapper for MonetDB"

PG_CPPFLAGS = -I$(libpq_srcdir)
SHLIB_LINK_INTERNAL = $(libpq)

EXTENSION = monetdb_fdw
DATA = monetdb_fdw--1.0.sql

REGRESS = monetdb_fdw

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
SHLIB_PREREQS = submake-libpq
subdir = contrib/monetdb_fdw
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

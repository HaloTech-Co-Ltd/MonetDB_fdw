# contrib/monetdb_fdw/Makefile

MODULE_big = monetdb_fdw
OBJS = \
	$(WIN32RES)   \
	deparse.o 	  \
	monetdb_fdw.o \
	shippable.o   \
	connection.o

PGFILEDESC = "monetdb_fdw - foreign data wrapper for MonetDB"

PG_CPPFLAGS = -I"$(MONETDB_HOME)/include/monetdb/"
SHLIB_LINK_INTERNAL = -L"$(MONETDB_HOME)/" -L"$(MONETDB_HOME)/lib64/" -lmapi-11.54.0

EXTENSION = monetdb_fdw
DATA = monetdb_fdw--1.0.sql

REGRESS = monetdb_fdw type_support

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/monetdb_fdw
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

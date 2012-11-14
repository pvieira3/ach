ACLOCAL_AMFLAGS = -I m4

EXTRA_DIST = CMakeLists.txt spin
dist_bin_SCRIPTS = achpipe achlog achtooltest src/run-benchmarks

AM_CPPFLAGS = -I$(top_srcdir)/include

TESTS = achtest achtooltest

include_HEADERS = include/ach.h

lib_LTLIBRARIES = libach.la

if HAVE_RT
bin_PROGRAMS = ach achpipe.bin achcat achbench
noinst_PROGRAMS = achtest achd ach-example
else
bin_PROGRAMS = ach achpipe.bin
noinst_PROGRAMS = achtest
endif

libach_la_SOURCES = src/ach.c src/pipe.c

# This is a libtool version -- CURRENT:REVSION:AGE
# Is /NOT/ major.minor.patch and the relationship is nontrivial
libach_la_LDFLAGS = -version-info 1:0:0

ach_SOURCES = src/achtool.c
ach_LDADD = libach.la

achpipe_bin_SOURCES = src/achpipe-bin.c
achpipe_bin_LDADD = libach.la

achtest_SOURCES = src/achtest.c
achtest_LDADD = libach.la

if HAVE_RT

achcat_SOURCES = src/achcat.c
achcat_LDADD = libach.la

achd_SOURCES = src/achd.c
achd_LDADD = libach.la

ach_example_SOURCES = src/ach-example.c
ach_example_LDADD = libach.la

achbench_SOURCES = src/ach-bench.c
achbench_LDADD = libach.la

endif


clean-local:
	-rm -rf debian/tmp debian/files debian/libach? debian/libach-dev debian/*.log debian/*.substvars texput.log debian/*.debhelper debian/usr/* debian/DEBIAN/*

include doxygen.am
# nghttp2 - HTTP/2 C Library

# Copyright (c) 2012 Tatsuhiro Tsujikawa

# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:

# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
SUBDIRS = includes

bin_PROGRAMS =
check_PROGRAMS =
TESTS =

AM_CFLAGS = $(WARNCFLAGS)
AM_CPPFLAGS = \
	-Wall \
	-I$(top_srcdir)/lib/includes \
	-I$(top_builddir)/lib/includes \
	-I$(top_srcdir)/lib \
	-I$(top_srcdir)/src/includes \
	-I$(top_srcdir)/third-party \
	@LIBSPDYLAY_CFLAGS@ \
	@XML_CPPFLAGS@	\
	@LIBEV_CFLAGS@ \
	@OPENSSL_CFLAGS@ \
	@JANSSON_CFLAGS@ \
	@ZLIB_CFLAGS@ \
	@DEFS@
AM_LDFLAGS = \
	@JEMALLOC_LIBS@ \
	@LIBSPDYLAY_LIBS@ \
	@XML_LIBS@ \
	@LIBEV_LIBS@ \
	@OPENSSL_LIBS@ \
	@JANSSON_LIBS@ \
	@ZLIB_LIBS@ \
	@APPLDFLAGS@

LDADD = \
	$(top_builddir)/lib/libnghttp2.la \
	$(top_builddir)/third-party/libhttp-parser.la

if ENABLE_APP

bin_PROGRAMS += nghttp nghttpd nghttpx

HELPER_OBJECTS = util.cc \
	http2.cc timegm.c app_helper.cc nghttp2_gzip.c
HELPER_HFILES = util.h \
	http2.h timegm.h app_helper.h nghttp2_config.h \
	nghttp2_gzip.h

HTML_PARSER_OBJECTS =
HTML_PARSER_HFILES = HtmlParser.h

if HAVE_LIBXML2
HTML_PARSER_OBJECTS += HtmlParser.cc
endif # HAVE_LIBXML2

nghttp_SOURCES = ${HELPER_OBJECTS} ${HELPER_HFILES} nghttp.cc nghttp.h \
	${HTML_PARSER_OBJECTS} ${HTML_PARSER_HFILES} \
	ssl.cc ssl.h

nghttpd_SOURCES = ${HELPER_OBJECTS} ${HELPER_HFILES} nghttpd.cc \
	ssl.cc ssl.h \
	HttpServer.cc HttpServer.h \
	ringbuf.h

bin_PROGRAMS += h2load

h2load_SOURCES = util.cc util.h \
	http2.cc http2.h h2load.cc h2load.h \
	timegm.c timegm.h \
	ssl.cc ssl.h \
	h2load_session.h \
	h2load_http2_session.cc h2load_http2_session.h

if HAVE_SPDYLAY
h2load_SOURCES += h2load_spdy_session.cc h2load_spdy_session.h
endif # HAVE_SPDYLAY

NGHTTPX_SRCS = \
	util.cc util.h http2.cc http2.h timegm.c timegm.h base64.h \
	app_helper.cc app_helper.h \
	ssl.cc ssl.h \
	shrpx_config.cc shrpx_config.h \
	shrpx_error.h \
	shrpx_accept_handler.cc shrpx_accept_handler.h \
	shrpx_connection_handler.cc shrpx_connection_handler.h \
	shrpx_client_handler.cc shrpx_client_handler.h \
	shrpx_upstream.h \
	shrpx_http2_upstream.cc shrpx_http2_upstream.h \
	shrpx_https_upstream.cc shrpx_https_upstream.h \
	shrpx_downstream.cc shrpx_downstream.h \
	shrpx_downstream_connection.cc shrpx_downstream_connection.h \
	shrpx_http_downstream_connection.cc shrpx_http_downstream_connection.h \
	shrpx_http2_downstream_connection.cc shrpx_http2_downstream_connection.h \
	shrpx_http2_session.cc shrpx_http2_session.h \
	shrpx_downstream_queue.cc shrpx_downstream_queue.h \
	shrpx_log.cc shrpx_log.h \
	shrpx_http.cc shrpx_http.h \
	shrpx_io_control.cc shrpx_io_control.h \
	shrpx_ssl.cc shrpx_ssl.h \
	shrpx_worker.cc shrpx_worker.h \
	shrpx_worker_config.cc shrpx_worker_config.h \
	shrpx_connect_blocker.cc shrpx_connect_blocker.h \
	shrpx_downstream_connection_pool.cc shrpx_downstream_connection_pool.h \
	shrpx_rate_limit.cc shrpx_rate_limit.h \
	ringbuf.h memchunk.h

if HAVE_SPDYLAY
NGHTTPX_SRCS += shrpx_spdy_upstream.cc shrpx_spdy_upstream.h
endif # HAVE_SPDYLAY

noinst_LIBRARIES = libnghttpx.a
libnghttpx_a_SOURCES = ${NGHTTPX_SRCS}

nghttpx_SOURCES = shrpx.cc shrpx.h
nghttpx_LDFLAGS =
nghttpx_LDADD = libnghttpx.a ${LDADD} ${AM_LDFLAGS}

if HAVE_CUNIT
check_PROGRAMS += nghttpx-unittest
nghttpx_unittest_SOURCES = shrpx-unittest.cc \
	shrpx_ssl_test.cc shrpx_ssl_test.h \
	shrpx_downstream_test.cc shrpx_downstream_test.h \
	shrpx_config_test.cc shrpx_config_test.h \
	http2_test.cc http2_test.h \
	util_test.cc util_test.h \
	nghttp2_gzip_test.c nghttp2_gzip_test.h \
	nghttp2_gzip.c nghttp2_gzip.h \
	ringbuf_test.cc ringbuf_test.h \
	memchunk_test.cc memchunk_test.h
nghttpx_unittest_CPPFLAGS = ${AM_CPPFLAGS}\
	 -DNGHTTP2_TESTS_DIR=\"$(top_srcdir)/tests\"
nghttpx_unittest_LDFLAGS = ${AM_LDFLAGS} \
	@CUNIT_LIBS@ @TESTLDFLAGS@
nghttpx_unittest_LDADD = libnghttpx.a ${LDADD}

TESTS += nghttpx-unittest
endif # HAVE_CUNIT

endif # ENABLE_APP

if ENABLE_HPACK_TOOLS

bin_PROGRAMS += inflatehd deflatehd

HPACK_TOOLS_COMMON_SRCS = comp_helper.c comp_helper.h

inflatehd_SOURCES = inflatehd.cc $(HPACK_TOOLS_COMMON_SRCS)

deflatehd_SOURCES = deflatehd.cc $(HPACK_TOOLS_COMMON_SRCS)

endif # ENABLE_HPACK_TOOLS

if ENABLE_ASIO_LIB

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libnghttp2_asio.pc
DISTCLEANFILES = $(pkgconfig_DATA)

lib_LTLIBRARIES = libnghttp2_asio.la

libnghttp2_asio_la_SOURCES = \
	asio_connection.h \
	asio_server.cc asio_server.h \
	asio_io_service_pool.cc asio_io_service_pool.h \
	asio_http2_handler.cc asio_http2_handler.h \
	asio_http2_impl.cc asio_http2_impl.h \
	util.cc util.h http2.cc http2.h \
	ssl.cc ssl.h

libnghttp2_asio_la_CPPFLAGS= ${AM_CPPFLAGS} ${BOOST_CPPFLAGS}
libnghttp2_asio_la_LDFLAGS = \
	${BOOST_LDFLAGS} \
	${BOOST_ASIO_LIB} \
	${BOOST_THREAD_LIB} \
	${BOOST_SYSTEM_LIB} \
	@OPENSSL_LIBS@ \
	-no-undefined \
	-version-info 0:0:0
libnghttp2_asio_la_LIBADD = $(top_builddir)/lib/libnghttp2.la

endif # ENABLE_ASIO_LIB

#
#  Copyright 2021 Northern.tech AS
#
#  This file is part of CFEngine 3 - written and maintained by Northern.tech AS.
#
#  This program is free software; you can redistribute it and/or modify it
#  under the terms of the GNU General Public License as published by the
#  Free Software Foundation; version 3.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
#
# To the extent this program is licensed as part of the Enterprise
# versions of CFEngine, the applicable Commercial Open Source License
# (COSL) may apply to this file if you as a licensee so wish it. See
# included file COSL.txt.
#
noinst_LTLIBRARIES = libcfnet.la

AM_CPPFLAGS = -I$(top_srcdir)/libntech/libutils \
	-I$(top_srcdir)/libpromises \
	$(PCRE2_CPPFLAGS) \
	$(SYSTEMD_SOCKET_CPPFLAGS) \
	$(LIBRSYNC_CPPFLAGS) \
	$(OPENSSL_CPPFLAGS)

libcfnet_la_SOURCES = \
	addr_lib.c addr_lib.h \
	client_protocol.c client_protocol.h cfnet.h\
	client_code.c client_code.h \
	classic.c classic.h \
	communication.c communication.h \
	connection_info.c connection_info.h \
	conn_cache.c conn_cache.h \
	file_stream.c file_stream.h \
	key.c key.h \
	misc.c \
	net.c net.h \
	policy_server.c policy_server.h \
	protocol.c protocol.h \
	protocol_version.c protocol_version.h \
	server_code.c server_code.h \
	stat_cache.c stat_cache.h \
	tls_client.c tls_client.h \
	tls_generic.c tls_generic.h

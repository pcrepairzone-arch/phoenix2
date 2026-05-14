#
# Copyright (c) 2019, RISC OS Open Ltd
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.
#     * Neither the name of RISC OS Open Ltd nor the names of its contributors
#       may be used to endorse or promote products derived from this software
#       without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#
# Makefile for EtherGENET
#

COMPONENT    = EtherGENET
TARGET      ?= EtherGENET
OBJS         = o.egemodule o.bcmgenet o.glue o.filtering o.filteringasm o.mbuf6 o.internet6
CMHGDEPENDS  = egemodule glue
ROMCDEFINES  = -DROM
RES_AREA     = etherge_messages
HDRS         =
INCLUDES     = ^.^.Lib,^.^.obsd.Lib,^.^.obsd.Lib.libc,^.^.build,TCPIPLibs:
LIBS        += ${NET5LIBS} ${ASMUTILS} ${SYNCLIB}
CUSTOMSA     = custom
VPATH = ^.EtherGENET-6 ^.^.obsd.libc.gen ^.^.build

include CModule

CFLAGS      += -Wp -DRISCOS -DINET -D_KERNEL -DMBCTL_CLIENT=mbctl6 
CDFLAGS     += -DDEBUG

#
# Specials for install (and standalone because they use the same override)
#
standalone_custom: ${RMDIR}.${SA_TARGET}
	@${ECHO} ${COMPONENT}: ram module built

install_custom: install_${TARGET}; @

install_EtherGENET: ${RMDIR}.${SA_TARGET}
	${MKDIR} ${INSTDIR}
	${CP} ${RMDIR}.${SA_TARGET} ${INSTDIR}.${TARGET} ${CPFLAGS}
	@${ECHO} ${COMPONENT}: ram module installed

install_AutoSense: AutoSense.${COMPONENT}
	${CP} AutoSense.${COMPONENT} ${INSTDIR}.${COMPONENT} ${CPFLAGS}
	@${ECHO} ${COMPONENT}: AutoSense file installed

# Dynamic dependencies:

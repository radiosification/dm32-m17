// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The net view section: the radio's own services, TCP and its services (docs/spec/tcp-services.md); "net.built=0"
// without any of them. The net_svc section: mDNS and TFTP (mdns.md, tftp.md)
#include "config.h"
#include "debuglink/debuglink.h"

void debuglink_view_net(dl_text_t *t)
{
	dl_text(t, "net.built=0\n");
}

// Its own section: with the TCP services' keys the net section outgrew a reply (truncated=1 before mdns.*)
void debuglink_view_net_svc(dl_text_t *t)
{
	dl_text(t, "net_svc.built=0\n");
}

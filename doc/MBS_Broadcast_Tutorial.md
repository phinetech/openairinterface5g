<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# MBS broadcast tutorial

> This work is part of the ESA activity **AD-SAT** (*Provision of Broadband
> Control Centre Services for Autonomous Driving over Satellite
> Constellations*, activity 3F.049) with Frequentis AG. See the
> [project page](https://connectivity.esa.int/?p=26285).

One gNB sends a single transmission on a G-RNTI and three UEs receive it.
Unlike [MBS.md](./MBS.md), which drives the group from a test adapter, here
the session comes from the core: the MB-SMF creates a broadcast session, the
gNB learns it over NGAP, and the payload arrives on the session's own
multicast tunnel.

[[_TOC_]]

## Where this is today

MBS has two service types. A **broadcast** session is sent to everyone in the
service area: receivers do not join, the network does not know they exist,
and they find the session by reading what the cell announces, so they can be
idle. A **multicast** session is sent only to UEs that joined it. They are
known individually, which is what makes HARQ feedback and retransmission
possible, and each is told about the session in its own RRCReconfiguration.

What runs here is broadcast:

* **The core creates a broadcast session.** It allocates a TMGI, distributes
  the session to a multicast group with one common TEID shared by every gNB
  in the area, and announces it with `BroadcastSessionSetup` over NGAP.
* **The cell announces it.** SIB20 lists the sessions of the cell, each with
  its TMGI, its G-RNTI and the configuration of its radio bearer, and gives
  the CORESET of the PDCCHs; SIB1 names the search space.
* **Receivers configure themselves** from that, with no signalling of their
  own. The gNB does not know who receives, transmits whether or not anybody
  is connected, and a UE keeps receiving in RRC_IDLE.

HARQ feedback is off, because a broadcast session has no members to report
with, and the session is sent at a fixed MCS for the same reason: 9 unless
`mbs_broadcast_mcs` in the `MACRLCs` section says otherwise. Multicast,
where feedback is on, is described in [MBS.md](./MBS.md).

The remaining specification gaps are listed under
[Known gaps](#known-gaps).

## What this needs

* **A core that can create an MBS session** — see the next section. Upstream
  Open5GS cannot: it has no MB-SMF, so there is nothing to allocate a TMGI or
  set up the shared tunnel.
* **A monolithic gNB.** The MRB's PDCP PDUs have to reach the RLC entity that
  transmits them, and `nr_rlc_mrb_recv_sdu()` only works when PDCP and RLC
  run in the same process. Neither a CU/DU nor a CU-CP/CU-UP split can carry
  the MRB payload yet. Broadcast setup signalling over F1AP and E1AP is
  implemented; the missing part is MRB context coordination and F1-U data
  delivery, as described in [MBS.md](./MBS.md#implemented-broadcast-integration).
* **SIB20 switched on in the gNB**, with `du_sibs = [20];` in its
  configuration. A cell that schedules any other SIB also has to schedule one
  of SIB2 to SIB14, hence `cu_sibs = [2];` next to it. The configuration file
  of this tutorial has both.
* Three subscribers, IMSI `999700123456701`, `702` and `703`. They are in
  `subscribers.json` next to the compose file and are imported automatically.

## The core network

A ready image of the core described below is on Docker Hub:

```bash
docker pull phinetech/open5gs_core:mbs
docker tag phinetech/open5gs_core:mbs open5gs_core:mbs
```

With it, continue at [Build the images](#build-the-images). To build the core
yourself, use the 5G-MAG fork of Open5GS, which adds the MBS functions:

```bash
git clone --branch 5mbs-development https://github.com/5G-MAG/open5gs.git
```

Build it the way Open5GS documents (`meson` and `ninja`); the fork adds no
extra steps. The MBS functions are not separate daemons — the MB-SMF and
MB-UPF are merged into `open5gs-smfd` and `open5gs-upfd`, which is why the
session API below is served by the SMF.

**Do not mix binaries with upstream Open5GS.** The fork changes shared
libraries in incompatible ways, and because every daemon loads the same ones,
an upstream binary next to the fork's libraries will crash at startup. Build
everything from the fork.

For MBS you need nine network functions:

```
open5gs-nrfd  open5gs-scpd  open5gs-amfd  open5gs-smfd  open5gs-upfd
open5gs-ausfd open5gs-udmd  open5gs-udrd  open5gs-pcfd
```

`open5gs-pcfd` is easy to overlook and not optional: the AMF asks it for an AM
policy during registration, and without it every UE is rejected with cause 95,
"semantically incorrect message", which says nothing about the real problem.
It also needs MongoDB to be up before it starts, so the compose file waits for
the database to answer before starting the core.

The core also needs a `tun` interface for the user plane (`ogstun`, as
Open5GS documents). The MB-UPF routes the session's multicast group into it
by itself (`upf.mbs.multicast_router` in `core.yaml`); for that it needs the
kernel's multicast routing to itself, so do not run a routing daemon such as
`smcrouted` next to it.

Package the result however you like. The compose file expects the image as
`open5gs_core:mbs`; to use another name, set `CN_IMG`. What it expects of
that image:

* the core reachable at `192.168.77.10`, with the AMF listening for NGAP and
  the SMF's service-based interface on `127.0.0.4:7777`;
* its configuration read from `/container/open5gs/core.yaml`, which the
  compose file mounts from `core.yaml` next to it. That file is an ordinary
  Open5GS configuration; if your packaging reads its configuration somewhere
  else, change the mount to match.

The subscribers are imported into MongoDB on first start, by
`provision-subscribers.sh` in the same directory.

## Build the images

The gNB and UE images are built on the shared `ran-base` and `ran-build`
images; see [docker/README.md](../docker/README.md).

```bash
cd ~/openairinterface5g
docker build . -f docker/Dockerfile.base.ubuntu  --tag ran-base:latest
docker build . -f docker/Dockerfile.build.ubuntu --tag ran-build:latest
docker build . -f docker/Dockerfile.gNB.ubuntu   --tag oai-gnb:mbs
docker build . -f docker/Dockerfile.nrUE.ubuntu  --tag oai-nr-ue:mbs
```

The compose file picks those up through `TAG`, which defaults to `mbs`.

## Start everything

```bash
cd ci-scripts/yaml_files/5g_rfsimulator_mbs_broadcast
docker compose up -d
```

Wait until each UE has an address:

```bash
for n in 1 2 3; do docker exec mbs-ue$n ip -4 addr show oaitun_ue1; done
```

If no `oaitun_ue1` appears and the UE log ends in a registration reject,
check the core first — see the note on `open5gs-pcfd` above.

## Create the broadcast session

The MB-SMF allocates the TMGI and tells the gNB over NGAP. The application
server `mbs-as` (`192.168.77.20`) sends to `239.0.0.25`; together they are the
source-specific multicast address of the session.

```bash
docker exec mbs-cn curl -s --http2-prior-knowledge -X POST \
  -H 'Content-Type: application/json' \
  -d '{"mbsSession":{"ssm":{"sourceIpAddr":{"ipv4Addr":"192.168.77.20"},
       "destIpAddr":{"ipv4Addr":"239.0.0.25"}},"tmgiAllocReq":true,
       "serviceType":"BROADCAST","snssai":{"sst":1},
       "mbsServiceArea":{"tais":[{"plmnId":{"mcc":"999","mnc":"70"},"tac":"000001"}]}}}' \
  127.0.0.4:7777/nmbsmf-mbssession/v1/mbs-sessions
```

The gNB log now shows the session arriving and the group being created:

```
[NGAP]   Broadcast session setup: TMGI bed4da99f907 (6 octets)
[NGAP]   Broadcast session user plane: group 239.0.0.4 source 192.168.77.10 C-TEID 0x00002b17
[NGAP]   Broadcast session setup acknowledged
[E1AP]   MBS: broadcast bearer context for TMGI bed4da99f907, MRB 1, SST 1
[GTPU]   MBS: joined group 239.0.0.4 from 192.168.77.10 on 192.168.77.101, port 2152
[GTPU]   MBS: listening for the broadcast session on TEID 0x2b17
[RLC]    fb30: added PTM RLC entity of MRB 1 on LCID 32
[NR_MAC] added MBS broadcast session G-RNTI fb30: LCID 32, HARQ feedback disabled, PTM HARQ processes 0xf000, DCI 4_1
[PDCP]   10000fb30: added MRB 1 (SN size 12)
```

Nothing is sent to the UEs for it. Each one finds the session the next time
it reads SIB20, within a second or two, and says so in its own log:

```
[RLC]    0000: added PTM RLC entity of MRB 32 on LCID 32
[PDCP]   0: added MRB 32 (SN size 12)
[NR_RRC] MBS: broadcast session with service ID bed4da announced on G-RNTI fb30, MTCH on LCID 32
```

Note the group address the core chose (`239.0.0.4` above) is not the one the
application sends to: the core distributes the session to a group of its own.

## Send traffic and watch it arrive

What a UE receives on a broadcast session is written to its broadcast
interface, `oaimbs_ue1` (address 169.254.0.1, link-local and not routed): a
broadcast session belongs to no PDU session, so it does not go to
`oaitun_ue1`. Counting the packets the interface took in is enough. Read the
counter before sending:

```bash
for n in 1 2 3; do
  docker exec mbs-ue$n cat /sys/class/net/oaimbs_ue1/statistics/rx_packets > /tmp/ue$n.before
done
```

Send twenty packets from the application server:

```bash
for i in $(seq 20); do
  docker exec mbs-as sendip -p ipv4 -is 192.168.77.20 -id 239.0.0.25 239.0.0.25
  sleep 0.2
done
```

Count what each UE received:

```bash
for n in 1 2 3; do
  after=$(docker exec mbs-ue$n cat /sys/class/net/oaimbs_ue1/statistics/rx_packets)
  echo "ue$n: $((after - $(cat /tmp/ue$n.before))) of 20"
done
```

All three receive the same packets from **one** transmission on the G-RNTI,
and the three counts are the same for that reason.

Expect 20 of 20. Broadcast has no feedback: the gNB does not know who is
listening, so nothing is retransmitted and what is lost stays lost.
Multicast is where ACK/NACK and retransmission belong; see
[MBS.md](./MBS.md).

To check the first batch automatically, run `python3 ci-scripts/tests/mbs-broadcast.py`
from the repository root instead of the counter and send loops above. Use a fresh
session, wait for all three UEs to announce it, and send no warm-up traffic: the
check exercises the first reuse of the broadcast HARQ processes.

The gNB counts what it sent in its periodic statistics, and does so with no
UE in the cell as well:

```
MBS broadcast session G-RNTI fb30: 20 PTM transmissions, 460 bytes, MCS 9
```

## Release the session

Delete the session at the core:

```bash
docker exec mbs-cn curl -s --http2-prior-knowledge -X DELETE \
  127.0.0.4:7777/nmbsmf-mbssession/v1/mbs-sessions/1
```

The core sends `BroadcastSessionReleaseRequest`. The gNB leaves the group and
removes the session from SIB20, and every UE stops receiving it the next time
it reads SIB20:

```
[NGAP]   Broadcast Session Release initiating message
[E1AP]   MBS: broadcast user plane released, TEID 0x2b17
[NR_MAC] MBS: broadcast session for TMGI bed4da99f907 released, G-RNTI f738
[NR_RRC] MBS: broadcast session with MTCH on LCID 32 not announced anymore
```

A session created afterwards is announced and received like the first one.
The gNB can also release a session without the core, through the telnet
command `rrc release_mbs_broadcast`.

## Stop

```bash
docker compose down -v
```

`-v` throws away the MongoDB volume, so the subscribers are imported again on
the next start.

## Known gaps

* **There is no MCCH.** The list of sessions belongs on it, and SIB20 should
  only say where it is. Until it exists the message that goes on the MCCH,
  `MBSBroadcastConfiguration`, is carried inside SIB20, and the UE reads
  SIB20 again every 1.28 s to follow it.
* **The PDCCH uses DCI format 4_1**, the multicast one, where broadcast has
  format 4_0, and a session takes its HARQ processes from the ones of unicast
  rather than having a broadcast process of its own. The gNB therefore keeps
  four HARQ processes of every connected UE away from unicast while a
  session exists.
* **An OAI UE whose registration is rejected stops reading system
  information**, so it does not find a broadcast session either, although
  the broadcast interface needs no registration.

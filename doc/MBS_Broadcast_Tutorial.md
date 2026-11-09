<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# MBS broadcast tutorial

One gNB sends a single transmission on a G-RNTI and three UEs receive it.
Unlike [MBS.md](./MBS.md), which drives the group from a test adapter, here
the session comes from the core: the MB-SMF creates a broadcast session, the
gNB learns it over NGAP, and the payload arrives on the session's own
multicast tunnel.

[[_TOC_]]

## Where this is today

MBS has two service types, and this setup is a mixture of them. It is worth
knowing which half is which before reading the rest.

A **broadcast** session is sent to everyone in the service area. Receivers do
not join and the network does not know they exist, so they can be idle, and
they find the session by reading it from the system information. A
**multicast** session is sent only to UEs that joined it. They are known
individually, which is what makes HARQ feedback and retransmission possible,
and each is told about the session in its own RRCReconfiguration.

What runs here:

* **The session is a broadcast session.** The core allocates a TMGI and
  distributes it to a multicast group with one common TEID shared by every
  gNB in the area, and announces it with `BroadcastSessionSetup` over NGAP.
  Nothing tracks members in the core.
* **Delivery is the standard shared one** — a single transmission on a
  G-RNTI with DCI format 4_1 in a common search space, carried on an MRB
  through a point-to-multipoint RLC entity. Broadcast and multicast use the
  same mechanism here.
* **Receivers are configured one by one, which is the multicast way.** The
  G-RNTI reaches each UE in its own RRCReconfiguration rather than in SIB20
  and the MCCH. Two consequences follow: every receiver has to be connected,
  and a cell with no UE attached transmits nothing.

So the control plane and the data path are broadcast, and only the last step
— how a UE finds out the group exists — is not. Announcing the session in
the system information is what remains, and it is also what would let an
idle UE receive.

HARQ feedback is switched off, because a broadcast session has no members to
report with. Multicast, where it is on, is described in [MBS.md](./MBS.md).

## What this needs

* **A core that can create an MBS session** — see the next section. Upstream
  Open5GS cannot: it has no MB-SMF, so there is nothing to allocate a TMGI or
  set up the shared tunnel.
* **A monolithic gNB.** The MRB's PDCP PDUs have to reach the RLC entity that
  transmits them, and `nr_rlc_mrb_recv_sdu()` only works when PDCP and RLC
  run in the same process. Neither a CU/DU nor a CU-CP/CU-UP split can carry
  this yet.
* Three subscribers, IMSI `999700123456701`, `702` and `703`. They are in
  `subscribers.json` next to the compose file and are imported automatically.

## The core network

Use the 5G-MAG fork of Open5GS, which adds the MBS functions:

```bash
git clone --branch 5mbs https://github.com/5G-MAG/open5gs.git
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

Two more things the core needs for this tutorial: a `tun` interface for the
user plane (`ogstun`, as Open5GS documents), and a multicast routing daemon
such as `smcrouted`, used below to route the group into that interface.

Package the result however you like and point the compose file at it with
`CN_IMG`. What the compose file expects of that image:

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

The MB-SMF allocates the TMGI and tells the gNB over NGAP. `239.0.0.25` is
the address the application sends to.

```bash
docker exec mbs-cn curl -s --http2-prior-knowledge -X POST \
  -H 'Content-Type: application/json' \
  -d '{"mbsSession":{"ssm":{"sourceIpAddr":{"ipv4Addr":"192.168.77.10"},
       "destIpAddr":{"ipv4Addr":"239.0.0.25"}},"tmgiAllocReq":true,
       "serviceType":"BROADCAST"}}' \
  127.0.0.4:7777/nmbsmf-mbssession/v1/mbs-sessions
```

Route the group into the core's tunnel interface:

```bash
docker exec mbs-cn smcroutectl add eth0 239.0.0.25 ogstun
```

The gNB log now shows the session arriving and the group being created:

```
[NGAP]   Broadcast session setup: TMGI 202621000000 (6 octets)
[NGAP]   Broadcast session user plane: group 239.0.0.4 source 192.168.55.5 C-TEID 0x00007742
[NGAP]   Broadcast session setup acknowledged
[E1AP]   MBS: broadcast bearer context for TMGI 202621000000, MRB 1, SST 1
[GTPU]   MBS: joined group 239.0.0.4 on 192.168.77.101, port 2152
[GTPU]   MBS: listening for the broadcast session on TEID 0x7742
[RLC]    fb30: added PTM RLC entity of MRB 1 on LCID 32
[NR_MAC] added MBS group G-RNTI fb30: LCID 32, HARQ feedback disabled, PTM HARQ processes 0xf000
[PDCP]   10000fb30: added MRB 1 (SN size 12)
[NR_MAC] UE 2f64 joined MBS group G-RNTI fb30 (1 members)
[NR_MAC] UE d8dc joined MBS group G-RNTI fb30 (2 members)
[NR_MAC] UE ff10 joined MBS group G-RNTI fb30 (3 members)
```

Note the group address the core chose (`239.0.0.4` above) is not the one the
application sends to: the core distributes the session to a group of its own.

## Send traffic and watch it arrive

What a UE receives on the group is written to `oaitun_ue1`, so counting the
packets the interface took in is enough; the UEs are otherwise idle. Read the
counter before sending:

```bash
for n in 1 2 3; do
  docker exec mbs-ue$n cat /sys/class/net/oaitun_ue1/statistics/rx_packets > /tmp/ue$n.before
done
```

Send twenty packets from the core:

```bash
for i in $(seq 20); do
  docker exec mbs-cn sendip -p ipv4 -is 192.168.77.10 -id 239.0.0.25 239.0.0.25
  sleep 0.2
done
```

Count what each UE received:

```bash
for n in 1 2 3; do
  after=$(docker exec mbs-ue$n cat /sys/class/net/oaitun_ue1/statistics/rx_packets)
  echo "ue$n: $((after - $(cat /tmp/ue$n.before))) of 20"
done
```

All three receive the same packets from **one** transmission on the G-RNTI,
and the three counts are the same for that reason.

Expect 15 to 20 of 20; it varies from run to run. Broadcast has no feedback: the
gNB does not know who is listening, so nothing is retransmitted and what is
lost stays lost. Multicast is where ACK/NACK and retransmission belong; see
[MBS.md](./MBS.md).

## Stop

```bash
docker compose down -v
```

`-v` throws away the MongoDB volume, so the subscribers are imported again on
the next start.

## Known gaps

Besides the configuration of the receivers described at the top, the UE
writes what it receives to the tun of its first PDU session rather than to
the session's multicast address.

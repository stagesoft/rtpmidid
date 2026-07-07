# rtpmidid

Part of the **CUEMS** ecosystem — see the [`cuems-RELATIONS`](https://github.com/stagesoft/cuems-RELATIONS) repo for the system index, architecture diagram, and protocol/port map.

## Role

Stagesoft's RTP-MIDI (AppleMIDI) daemon (fork). Bridges ALSA MIDI ↔ network RTP-MIDI and **distributes MTC across the cluster** so every node's `Midi Through Port-0` sees the controller's timecode. Peers are discovered via avahi `_apple-midi._udp.local`; timecode rides UDP :5004. C++. Service `rtpmidid.service`, `PartOf=cuems-node.target` via a cuems drop-in. Prebuilt `.deb` cached at `rc1_packages/rtpmidid_*.deb`.

## Role-aware config

cuems-common ships `/usr/share/cuems/rtpmidid/default.ini.{controller,node}` and a helper `cuems-write-rtpmidid-config` (rtpmidid's `ExecStartPre`) that picks the template by presence of `/etc/cuems/master.ip` and writes the runtime INI to `/run/cuems/rtpmidid.ini`. The cuems drop-in overrides ExecStart to `--ini=/run/cuems/rtpmidid.ini`, leaving the upstream conffile `/etc/rtpmidid/default.ini` untouched. The node template declares `[connect_to] hostname=controller.local name=controller`, so the controller's relay port appears in every node's ALSA seq as a port named `controller`.

Because the config is generated only at ExecStartPre, a **role flip requires restarting rtpmidid** (see the cuems-common role-flip procedure).

## Field notes / gotchas

- **Cold-boot avahi race (FIXED).** rtpmidid can start ~170ms *before* `avahi-daemon.service`; its avahi-client isn't connected yet so it defers registration ("No group to announce to...") and — pre-fix — **never made good on the deferred announce** when avahi later connected. Result: the controller published ZERO `_apple-midi._udp` services, so nodes couldn't discover the MTC-bearing auto-export and got no MTC (sessions look "up" but carry no MIDI; asymmetric — controller sees nodes, nodes don't see controller). Fix: `lib/mdns_rtpmidi.cpp` `client_callback()` now calls `announce_all()` on `AVAHI_CLIENT_S_RUNNING` (this is the one that actually fixes it), plus a cuems-common drop-in `rtpmidid.service.d/cuems-node-group.conf` with `Wants/After=avahi-daemon.service` (belt — do NOT add `network-online.target`, it creates an ordering cycle that deletes rtpmidid's start job). Shipped as **rtpmidid 26.06~1stagelab1** + cuems-common. Recovery on an affected running box: `systemctl restart rtpmidid` (controller-local players are unaffected — they read MTC from MtcMaster via ALSA). Red herrings ruled out: kernel multicast routes (avahi owns egress), `rtpmidid-cli router.connect` (export ignores router `send_to`).
- **Avahi hostname collision on cloned nodes → no MTC.** `/etc/avahi/avahi-daemon.conf` sets `host-name=<derived-from-clone-source>` (often the source's MAC). On a freshly imaged clone both hosts claim the same name → avahi auto-suffixes the second to `<name>-2` and `<name>.local` then resolves ambiguously. rtpmidid uses the *advertised* hostname from mDNS, so peering fails (`Error getting address info for <MAC>.local:5004`) even though `controller.local` resolves. Fix: set a UNIQUE `host-name=` per host (match `role_id`), then restart avahi-daemon + rtpmidid. **Note:** the MTC actually rides the controller's `Midi Through-Midi Through Port-0` `_apple-midi` export, NOT the `controller` (5004) control session — verify both connect (`Latency Midi Through-...`). Also: restarting avahi-daemon on the controller cascades a cuems-controller-engine restart (binds to avahi) → unloads the running project — treat as project-disrupting. Verify end-to-end on a node: `timeout 4 aseqdump -p 14:0 | grep -c quarter` (~100/s = MTC arriving).

The upstream README (`README.md`, `README.librtpmidid.md`) documents general rtpmidid/librtpmidi usage.

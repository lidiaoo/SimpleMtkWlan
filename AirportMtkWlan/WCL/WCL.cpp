//
//  WCL.cpp
//  AirportItlwm
//
//  Created by laobamac on 2026/10/1.
//  Copyright © 2026 laobamac. All rights reserved.
//

#ifdef AIRPORT_WCL
#include "../AirportItlwmV2.hpp"
#include "../AirportItlwmSkywalkInterface.hpp"
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <net80211/ieee80211_node.h>
#include <net80211/ieee80211_ioctl.h>
#include <net80211/ieee80211_priv.h>
#include <crypto/sha1.h>

extern IOCommandGate *_fCommandGate;

struct AirportItlwmSkywalkInterface::CountryUpdate {
    AirportItlwmSkywalkInterface *interface;
    AirportItlwm *controller;
    IOCommandGate *gate;
    IO80211WorkQueue *queue;
    thread_call_t call;
};

void AirportItlwmSkywalkInterface::deferWCLCountryUpdate()
{
    auto queue = instance->getWorkQueue();
    if (wclStopping || !_fCommandGate || !queue)
        return;
    auto update = static_cast<CountryUpdate *>(IOMalloc(sizeof(CountryUpdate)));
    if (!update)
        return;
    update->call = thread_call_allocate(wclCountryUpdateThread, update);
    if (!update->call) {
        IOFree(update, sizeof(*update));
        return;
    }
    update->interface = this;
    update->controller = instance;
    update->gate = _fCommandGate;
    update->queue = queue;
    retain();
    instance->retain();
    update->gate->retain();
    queue->retain();
    thread_call_enter(update->call);
}

void AirportItlwmSkywalkInterface::wclCountryUpdateThread(void *arg, void *)
{
    auto update = static_cast<CountryUpdate *>(arg);
    // Native country notifications synchronously query WCL. Holding the gate
    // is necessary, but executing on the driver's workloop thread is forbidden.
    update->gate->runAction(wclCountryUpdateGated, update);
    thread_call_free(update->call);
    update->interface->release();
    update->controller->release();
    update->gate->release();
    update->queue->release();
    IOFree(update, sizeof(*update));
}

IOReturn AirportItlwmSkywalkInterface::wclCountryUpdateGated(
    OSObject *, void *arg, void *, void *, void *)
{
    auto update = static_cast<CountryUpdate *>(arg);
    if (update->interface->wclStopping)
        return kIOReturnNotReady;
    if (!update->queue->inGate() || update->queue->onThread())
        return kIOReturnNotPermitted;
    update->interface->postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED, nullptr, 0, false);
    return kIOReturnSuccess;
}

bool AirportItlwmSkywalkInterface::initWCL()
{
    /*
     * PATCH [local-p26]: "-mtk-allwifi" publishes the whole candidate tree
     * instead of the strongest 40.  Presence of the flag is enough; no value
     * is read from it, matching the "-novht" / "-noht40" convention.
     */
    int allwifi = 0;
    wclPublishAll = PE_parse_boot_argn("-mtk-allwifi", &allwifi, sizeof(allwifi));
    if (wclPublishAll)
        XYLog("WCL: -mtk-allwifi set, scan list is not capped\n");
    joinTimer = IOTimerEventSource::timerEventSource(this, wclJoinTimeout);
    if (!joinTimer)
        return false;
    return instance->getWorkQueue()->addEventSource(joinTimer) == kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::stopWCL()
{
    // Pending country tasks must observe shutdown under the same gate.
    if (_fCommandGate)
        _fCommandGate->runAction(stopWCLGated, this);
    wclStopping = true;
    if (joinTimer) {
        joinTimer->cancelTimeout();
        if (joinTimer->getWorkLoop())
            joinTimer->getWorkLoop()->removeEventSource(joinTimer);
        OSSafeReleaseNULL(joinTimer);
    }
    if (scanSource)
        scanSource->cancelTimeout();
    scanPending = joinPending = false;
    joinDeferred = false;
    explicit_bzero(&deferredJoin, sizeof(deferredJoin));
    if (fHalService) {
        ieee80211com *ic = fHalService->get80211Controller();
        ic->ic_wcl_join_requested = false;
        ic->ic_wcl_scan_requested = false;
        explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
    }
}

IOReturn AirportItlwmSkywalkInterface::stopWCLGated(
    OSObject *, void *arg, void *, void *, void *)
{
    auto self = static_cast<AirportItlwmSkywalkInterface *>(arg);
    self->portAuthorized = false;
    self->updateWCLLinkQuality(true);
    self->wclStopping = true;
    return kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::updateWCLLinkQuality(bool force)
{
    if (_fCommandGate)
        _fCommandGate->runAction(updateWCLLinkQualityGated, this,
                                reinterpret_cast<void *>(uintptr_t(force)));
}

IOReturn AirportItlwmSkywalkInterface::updateWCLLinkQualityGated(
    OSObject *, void *arg0, void *arg1, void *, void *)
{
    auto self = static_cast<AirportItlwmSkywalkInterface *>(arg0);
    if (self->wclStopping || !self->fHalService)
        return kIOReturnNotReady;
    auto queue = self->instance->getWorkQueue();
    if (!queue || !queue->inGate())
        return kIOReturnNotPermitted;
    auto interface = self->mExpansionData ? self->getBSDInterface() : nullptr;
    if (!interface) {
        self->wclLinkQualityInterface = nullptr;
        self->wclReportedLinkQuality = -2;
        return kIOReturnNotReady;
    }
    auto ic = self->fHalService->get80211Controller();
    int quality = -2;
    if (self->portAuthorized && self->linkState() == kIO80211NetworkLinkUp &&
        (ifnet_flags(interface) & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING) &&
        ic && ic->ic_state == IEEE80211_S_RUN && ic->ic_bss &&
        IEEE80211_ADDR_EQ(ic->ic_bss->ni_bssid, self->joinBSSID)) {
        int rssi = IWM_MIN_DBM + ic->ic_bss->ni_rssi;
        quality = rssi >= -70 ? 100 : (rssi >= -85 ? 50 : 20);
    }
    if (arg1 || quality != self->wclReportedLinkQuality ||
        interface != self->wclLinkQualityInterface) {
        self->setLQM(static_cast<unsigned long long>(static_cast<long long>(quality)));
        self->wclReportedLinkQuality = quality;
        self->wclLinkQualityInterface = interface;
    }
    return kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::free()
{
    if (joinTimer)
        stopWCL();
    IO80211InfraProtocol::free();
}

IOReturn AirportItlwmSkywalkInterface::wclCommand(OSObject *, void *a0, void *a1, void *a2, void *)
{
    auto self = static_cast<AirportItlwmSkywalkInterface *>(a0);
    if (self->wclStopping)
        return kIOReturnNotReady;
    switch (uintptr_t(a1)) {
        case 16: return self->applyWCLRoamLock(a2);
        case 13: return self->copyWCLChannels(a2);
        case 14: return self->copyWCLTrafficCounters(a2);
        case 15:
            return self->fHalService->setScanHomeAwayTime(
                AirportWCL::read32(static_cast<const uint8_t *>(a2)));
        case 12:
            self->handleWCLEvent(int(uintptr_t(a2)), nullptr);
            return kIOReturnSuccess;
        case 11: self->clearWCLPMKSA(); return kIOReturnSuccess;
        case 8: return self->installWCLKey(static_cast<apple80211_key *>(a2));
        case 9: return self->accessWCLRSN(static_cast<apple80211_rsn_ie_data *>(a2), true);
        case 10: return self->accessWCLRSN(static_cast<apple80211_rsn_ie_data *>(a2), false);
        case 0: return self->beginWCLScan(static_cast<apple80211ScanRequest *>(a2));
        case 1: return self->beginWCLJoin(static_cast<apple80211AssocCandidates *>(a2));
        case 2: return self->abortWCLJoin();
        case 3:
            XYLog("WCL leave: airportd leaveNetwork cmd (state=%d)\n",
                  self->fHalService->get80211Controller()->ic_state);
            return self->leaveWCLNetwork();
        case 6: return self->copyWCLExtendedBss(a2);
        case 7:
            if (!a2 || !self->wclBssValid || !self->wclAssociatedBeacons)
                return kIOReturnNotReady;
            memcpy(a2, self->wclBeaconInfo, sizeof(self->wclBeaconInfo));
            return kIOReturnSuccess;
        case 5:
            if (!a2 || !self->wclBssValid)
                return kIOReturnNotReady;
            memcpy(a2, self->wclBssInfo, sizeof(self->wclBssInfo));
            return kIOReturnSuccess;
        case 4:
            self->finishWCLScan(kIOReturnAborted);
            return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_SCAN_REQ(apple80211ScanRequest *request)
{
    return _fCommandGate->runAction(wclCommand, this, nullptr, request);
}

IOReturn AirportItlwmSkywalkInterface::setWCL_SET_ROAM_LOCK(apple80211_set_roam_lock *data)
{
    if (!data)
        return kIOReturnBadArgument;
    if (!_fCommandGate)
        return kIOReturnNotReady;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(16), data);
}

IOReturn AirportItlwmSkywalkInterface::applyWCLRoamLock(void *data)
{
    if (!data)
        return kIOReturnBadArgument;
    if (!fHalService)
        return kIOReturnNotReady;
    // Native WCL consumes the first byte as bool and sets firmware roam_off.
    fHalService->get80211Controller()->ic_wcl_roam_locked =
        *static_cast<const uint8_t *>(data) != 0;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_SCAN_ABORT(void *)
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(4));
}

IOReturn AirportItlwmSkywalkInterface::setWCL_ASSOCIATE(apple80211AssocCandidates *request)
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(1), request);
}

#if __IO80211_TARGET == __MAC_15_2
IOReturn AirportItlwmSkywalkInterface::setWCL_JOIN_ABORT(void *)
#else
IOReturn AirportItlwmSkywalkInterface::setWCL_JOIN_ABORT(apple80211_wcl_abort_join *)
#endif
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(2));
}

IOReturn AirportItlwmSkywalkInterface::setWCL_LEAVE_NETWORK(apple80211_leave_network *)
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(3));
}

IOReturn AirportItlwmSkywalkInterface::beginWCLScan(apple80211ScanRequest *request)
{
    AirportWCL::ScanRequest scan;
    if (!AirportWCL::decodeScan(request, 0x1550, scan))
        return kIOReturnBadArgument;
    /*
     * DIAG [scan-5g] (local, not upstream): mt7921_mcu_hw_scan reports
     * "2g=14 5g=0" even when the adapter runs on a 5 GHz network, so the
     * "Other networks" list only ever shows 2.4 GHz. The bitmap we hand
     * to the HAL is built by decodeScan(), which drops entries on three
     * guessed flag rules. Dump the raw apple80211_channel list for the
     * first few requests so the real flag encoding can be read off the
     * machine instead of guessed. Pure logging: no behaviour change.
     */
    {
        static unsigned diagScans;

        if (diagScans < 6 && scan.channelCount) {
            const uint8_t *raw =
                reinterpret_cast<const uint8_t *>(request);
            unsigned kept = 0, drop6g = 0, dropBand = 0, dropRange = 0;

            for (uint32_t i = 0; i < scan.channelCount; ++i) {
                const uint8_t *entry = raw + 0x58 + size_t(i) * 12;
                uint32_t channel = AirportWCL::read32(entry + 4);
                uint32_t flags = AirportWCL::read32(entry + 8);

                if (!channel || channel > 196) {
                    dropRange++;
                    continue;
                }
                if (flags & 0x2000) {
                    drop6g++;
                    continue;
                }
                if (((flags & 0x8) && channel > 14) ||
                    ((flags & 0x10) && channel <= 14)) {
                    dropBand++;
                    continue;
                }
                kept++;
            }
            XYLog("WCL scan diag count=%u type=%u ssidLen=%u kept=%u "
                "dropRange=%u drop6g=%u dropBand=%u bm=%02x%02x%02x%02x\n",
                scan.channelCount, scan.type, scan.ssidLength, kept,
                dropRange, drop6g, dropBand, scan.channels[0],
                scan.channels[1], scan.channels[4], scan.channels[5]);
            for (uint32_t i = 0; i < scan.channelCount && i < 24; ++i) {
                const uint8_t *entry = raw + 0x58 + size_t(i) * 12;
                XYLog("WCL scan diag ch[%u] channel=%u flags=0x%x\n", i,
                    AirportWCL::read32(entry + 4),
                    AirportWCL::read32(entry + 8));
            }
            diagScans++;
        }
    }
    ieee80211com *ic = fHalService->get80211Controller();
    /*
     * PATCH [local-p7]: a channel walk that gets interrupted by a join (or
     * by a state change) can end without MCU_EVENT_SCAN_DONE ever arriving.
     * scanPending then stays latched and every later request is answered
     * with kIOReturnBusy: airportd reports "scanResultsCount=0" after
     * ~13 ms and the Wi-Fi menu stays empty until the interface is power
     * cycled.  Measured on 2026-11-08: after a disconnect/join storm the
     * driver sat in S_SCAN with scan request 42 / scan done 40 and refused
     * everything; setairportpower off/on was the only way out.
     *
     * So: repair the contradictory states instead of refusing forever, and
     * log which condition was taken so the ioreg ring shows what latched.
     * A healthy walk is capped at 12 s (scanSource timeout), so anything
     * older than 18 s + 4 s of slack is definitely orphaned.
     */
    {
        static uint32_t healLogs, rejectLogs;
        uint64_t now = airport_up_time();

        if (scanPending && wclScanDeadline != 0 &&
            now > wclScanDeadline + 4000) {
            if (healLogs < 24) {
                XYLog("WCL scan self-heal: orphaned scanPending "
                    "age=%llums results=%u state=%d idle=%d\n",
                    now - (wclScanDeadline - 18000), wclScanResults,
                    ic->ic_state, ic->ic_wcl_scan_idle);
                healLogs++;
            }
            scanPending = false;
            scanCompleted = false;
            scanQuietWaits = 0;
            wclScanResults = 0;
            scanSource->cancelTimeout();
            ic->ic_wcl_scan_requested = false;
        }
        /* A cache bgscan can only be live while associated; anywhere at or
         * below S_SCAN the flag is left over from an aborted transition. */
        if ((ic->ic_flags & IEEE80211_F_BGSCAN) &&
            ic->ic_state <= IEEE80211_S_SCAN) {
            if (healLogs < 24) {
                XYLog("WCL scan self-heal: stale bgscan flag in state %d\n",
                    ic->ic_state);
                healLogs++;
            }
            ic->ic_flags &= ~IEEE80211_F_BGSCAN;
        }
        if (!(ic->ic_if.if_flags & IFF_RUNNING)) {
            if (rejectLogs < 12) {
                XYLog("WCL scan reject: ifnet not running state=%d\n",
                    ic->ic_state);
                rejectLogs++;
            }
            return kIOReturnNotReady;
        }
        if (scanPending || joinPending ||
            (ic->ic_flags & IEEE80211_F_BGSCAN)) {
            if (rejectLogs < 12) {
                XYLog("WCL scan reject busy: scanPending=%d joinPending=%d "
                    "bgscan=%d state=%d idle=%d\n", scanPending, joinPending,
                    !!(ic->ic_flags & IEEE80211_F_BGSCAN), ic->ic_state,
                    ic->ic_wcl_scan_idle);
                rejectLogs++;
            }
            return kIOReturnBusy;
        }
        if (ic->ic_state == IEEE80211_S_AUTH ||
            ic->ic_state == IEEE80211_S_ASSOC) {
            if (rejectLogs < 12) {
                XYLog("WCL scan reject: authing state=%d\n", ic->ic_state);
                rejectLogs++;
            }
            return kIOReturnBusy;
        }
    }
    if (scan.channelCount) {
        bool usable = false;
        for (unsigned channel = 1; channel <= IEEE80211_CHAN_MAX; ++channel)
            if (ic->ic_channels[channel].ic_flags &&
                AirportWCL::scanChannelSelected(scan.channels, true, channel))
                usable = true;
        if (!usable)
            return kIOReturnUnsupported;
    }
    scanPending = true;
    scanCompleted = false;
    scanQuietWaits = 0;
    wclScanResults = 0;
    wclScanDeadline = airport_up_time() + 18000;
    scanSkipCompletion = ic->ic_state == IEEE80211_S_SCAN &&
        ((scan.type != 2 && scan.ssidLength != 0) ||
         ic->ic_wcl_scan_ssid_length != 0 || ic->ic_des_esslen != 0);
    ic->ic_wcl_scan_requested = true;
    ic->ic_wcl_scan_active = scan.type != 2;
    ic->ic_wcl_scan_ssid_length = scan.type == 2 ? 0 : scan.ssidLength;
    memcpy(ic->ic_wcl_scan_ssid, scan.ssid, sizeof(scan.ssid));
    ic->ic_wcl_scan_restricted = scan.channelCount != 0;
    memcpy(ic->ic_wcl_scan_channels, scan.channels, sizeof(scan.channels));
    /*
     * Hard cap only; the timer is re-armed to a short quiet period every time
     * a beacon is published, so a normal walk finishes seconds before this.
     */
    scanSource->setTimeoutMS(12000);
    if (ic->ic_state == IEEE80211_S_RUN) {
        ieee80211_begin_cache_bgscan(&ic->ic_if);
        if (!(ic->ic_flags & IEEE80211_F_BGSCAN)) {
            scanPending = false;
            ic->ic_wcl_scan_requested = false;
            scanSource->cancelTimeout();
            return kIOReturnBusy;
        }
    } else if (ic->ic_state != IEEE80211_S_SCAN) {
        int error = ic->ic_newstate(ic, IEEE80211_S_SCAN, -1);
        if (error) {
            scanPending = false;
            ic->ic_wcl_scan_requested = false;
            scanSource->cancelTimeout();
            return kIOReturnError;
        }
    } else if (ic->ic_wcl_scan_idle) {
        // The stack waits for us between scans (ieee80211_end_scan).
        ic->ic_wcl_scan_idle = false;
        scanSkipCompletion = false;
        ieee80211_next_scan(&ic->ic_if);
    }
    return kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::finishWCLScan(IOReturn status)
{
    if (!scanPending)
        return;
    /*
     * A scan that produced no beacon event still leaves the node tree full of
     * APs from the channel walk.  Reporting success with zero results makes
     * airportd replace its cache with an empty set, which is the "list goes
     * blank for a moment" flicker in the Wi-Fi menu.
     *
     * PATCH [local-p23]: the walk no longer publishes each beacon as it
     * arrives, so the full result set is handed over here in one burst,
     * strongest signal first.  scanPending has to be cleared before that or
     * publishWCLBeacon() would only count the nodes again instead of sending
     * them.
     */
    scanPending = false;
    /*
     * PATCH [local-p26]: one line per completion.  Two of these for a single
     * "scan request seq=" means the quiet timer and SCAN_DONE each finished
     * the walk; the bounded wait in completeWCLScan() should collapse them.
     */
    XYLog("WCL scan finish status=0x%x completed=%d results=%u waits=%u "
        "age=%llums\n", status, scanCompleted, wclScanResults, scanQuietWaits,
        airport_up_time() - (wclScanDeadline - 18000));
    publishWCLCachedResults();
    scanCompleted = false;
    scanQuietWaits = 0;
    wclScanResults = 0;
    scanSource->cancelTimeout();
    fHalService->get80211Controller()->ic_wcl_scan_requested = false;
    instance->postMessage(this, AirportWCL::ScanDone, &status, sizeof(status), true);
    /*
     * PATCH [local-p15]: replay a join that arrived while this walk was
     * still live.  scanPending is already cleared above, so the replayed
     * request takes the normal path instead of deferring again.
     */
    if (joinDeferred)
        resumeWCLJoin();
}

void AirportItlwmSkywalkInterface::completeWCLScan()
{
    if (wclStopping)
        return;
    /*
     * PATCH [local-p26]: the quiet period can expire while the firmware walk
     * is still on its last channels -- measured 0.68 s before SCAN_DONE.
     * Finishing there reported the request twice (a half-swept list first,
     * then the real one) and made airportd rebuild the menu both times.
     * Wait briefly for the real event; two 400 ms re-arms cover the measured
     * gap, and the hard deadline still ends a scan whose SCAN_DONE was
     * dropped, so nothing can hang.
     */
    if (!scanCompleted && scanQuietWaits < 2 &&
        airport_up_time() + 900 < wclScanDeadline) {
        ++scanQuietWaits;
        scanSource->setTimeoutMS(400);
        return;
    }
    /*
     * MT7921 does deliver MCU_EVENT_SCAN_DONE, but it is dropped whenever the
     * walk is started from a settled S_SCAN/S_RUN state or aborted early by a
     * join, so this timer is often the only completion.  IO80211 has already
     * ingested every beacon publishWCLBeacon() handed over, so once the walk
     * went quiet with something collected, reporting success is accurate.
     * The old code always reported kIOReturnTimeout in that case, which made
     * airportd discard the whole cache and redraw an empty list.
     */
    finishWCLScan((scanCompleted || wclScanResults) ? kIOReturnSuccess
                                                    : kIOReturnTimeout);
}

/*
 * PATCH [local-p25]: rebuild the information elements the way a real beacon
 * carries them.
 *
 * The node keeps parsed state rather than the raw frame, so the cache dump
 * used to re-serialise only the SSID and the RSN element.  Every entry then
 * reached airportd without a rate set, without DS parameters and without any
 * HT/VHT capability -- which shows up as "PHY Mode: None" in system_profiler.
 *
 * The visible cost was in the scan cache.  airportd splits a sweep into a
 * 2.4 GHz subset and a 5 GHz one; the 2.4 GHz half answered from cache
 * ("Successful cache-assisted"), but the 5 GHz half failed that test on every
 * pass and had to be scanned again, so the menu alternated between a
 * 2.4 GHz-only list and the full one.  A 5 GHz BSS is only allowed to be
 * 802.11n or better, hence the suspicion that airportd was refusing to cache
 * the 5 GHz entries for lack of the HT/VHT elements.
 *
 * Everything here comes out of the node itself (ieee80211_parse_htcap() and
 * friends filled it from the real frame), so the result describes the AP and
 * not this adapter.  A caller-visible rate set is included because airportd
 * derives the PHY mode from it.
 */
static size_t
wclBuildBeaconIEs(ieee80211com *ic, const struct ieee80211_node *ni,
    uint8_t *buf, size_t bufsize)
{
    uint8_t *frm = buf;
    uint8_t *limit = buf + bufsize - 64;    /* room for the RSN element */
    unsigned channel = ieee80211_chan2ieee(ic, ni->ni_chan);

    if (limit < buf || ni->ni_esslen > 32)
        return 0;

    frm = ieee80211_add_ssid(frm, ni->ni_essid, ni->ni_esslen);
    if (frm > limit)
        return 0;

    if (ni->ni_rates.rs_nrates > 0) {
        frm = ieee80211_add_rates(frm, &ni->ni_rates);
        if (ni->ni_rates.rs_nrates > IEEE80211_RATE_SIZE)
            frm = ieee80211_add_xrates(frm, &ni->ni_rates);
        if (frm > limit)
            return 0;
    }

    /*
     * PATCH [local-p29]: DS Params exists only in 2.4 GHz beacons; a real
     * 5 GHz beacon never carries it.  Emitting it for 5 GHz nodes produced
     * a malformed beacon and gave airportd a channel record that names a
     * 5 GHz channel inside a 2.4G-only element -- the kind of mismatch that
     * ends up in known-network profiles and later steers join-time directed
     * scans at the wrong band.
     */
    if (IEEE80211_IS_CHAN_2GHZ(ni->ni_chan)) {
        *frm++ = IEEE80211_ELEMID_DSPARMS;
        *frm++ = 1;
        *frm++ = uint8_t(channel);
        if (frm > limit)
            return 0;
    }

    if (ni->ni_flags & IEEE80211_NODE_HTCAP) {
        *frm++ = IEEE80211_ELEMID_HTCAPS;
        *frm++ = 26;
        LE_WRITE_2(frm, ni->ni_htcaps); frm += 2;
        *frm++ = ni->ni_ampdu_param;
        memcpy(frm, ni->ni_rxmcs, 10); frm += 10;
        LE_WRITE_2(frm, ni->ni_max_rxrate & IEEE80211_MCS_RX_RATE_HIGH);
        frm += 2;
        *frm++ = ni->ni_tx_mcs_set;
        *frm++ = 0; *frm++ = 0; *frm++ = 0;
        LE_WRITE_2(frm, ni->ni_htxcaps); frm += 2;
        LE_WRITE_4(frm, ni->ni_txbfcaps); frm += 4;
        *frm++ = ni->ni_aselcaps;
        if (frm > limit)
            return 0;

        *frm++ = IEEE80211_ELEMID_HTOP;
        *frm++ = 22;
        *frm++ = ni->ni_primary_chan ? ni->ni_primary_chan
                                    : uint8_t(channel);
        *frm++ = ni->ni_htop0;
        LE_WRITE_2(frm, ni->ni_htop1); frm += 2;
        LE_WRITE_2(frm, ni->ni_htop2); frm += 2;
        memset(frm, 0, 16); frm += 16;
        if (frm > limit)
            return 0;
    }

    if (ni->ni_flags & IEEE80211_NODE_VHTCAP) {
        *frm++ = IEEE80211_ELEMID_VHT_CAP;
        *frm++ = 12;
        LE_WRITE_4(frm, ni->ni_vhtcaps); frm += 4;
        LE_WRITE_2(frm, ni->ni_vht_mcsinfo.rx_mcs_map); frm += 2;
        LE_WRITE_2(frm, ni->ni_vht_mcsinfo.rx_highest); frm += 2;
        LE_WRITE_2(frm, ni->ni_vht_mcsinfo.tx_mcs_map); frm += 2;
        LE_WRITE_2(frm, ni->ni_vht_mcsinfo.tx_highest); frm += 2;
        if (frm > limit)
            return 0;

        *frm++ = IEEE80211_ELEMID_VHT_OPMODE;
        *frm++ = 5;
        *frm++ = ni->ni_vht_chanwidth;
        *frm++ = ni->ni_vht_chan1 ? ni->ni_vht_chan1 : uint8_t(channel);
        *frm++ = ni->ni_vht_chan2;
        LE_WRITE_2(frm, ni->ni_vht_basicmcs); frm += 2;
        if (frm > limit)
            return 0;
    }

    if (ni->ni_rsnprotos != 0) {
        frm = ieee80211_add_rsn(frm, ic, ni);
        if (frm > buf + bufsize)
            return 0;
    }

    return size_t(frm - buf);
}

void AirportItlwmSkywalkInterface::publishWCLCachedResults()
{
    ieee80211com *ic = fHalService->get80211Controller();
    struct ieee80211_node *ni;
    uint32_t nodes = 0, n2g = 0, n5g = 0;

    /*
     * Events only publish beacons that arrive while the scan is running.  A
     * scan started from a settled S_SCAN state finishes after the driver's
     * channel walk, so most of the APs sitting in ic_tree never produced a
     * beacon event during the window; the request then completed with
     * "success, 0 results" and the UI stayed empty.  Re-serialise the SSID and
     * RSN IEs from the node (the node keeps parsed state, not the raw frame)
     * and hand every candidate over before ScanDone.
     */
    /*
     * PATCH [local-p23]: two things made the menu list feel arbitrary and
     * jumpy.
     *
     * 1. The walk published every beacon from publishWCLBeacon() the moment
     *    it arrived, i.e. in channel order.  preinit() fills 2.4 GHz first,
     *    so the list was always built weak-2.4-first and the strong 5 GHz APs
     *    landed at the bottom -- exactly "the one with good signal is last".
     *    It also meant 120-156 separate ScanResult messages spread over the
     *    5.7 s walk, so any list opened mid-scan was rebuilt under the
     *    cursor.  Collection is now deferred to finishWCLScan() and sorted
     *    by RSSI, so the menu sees one burst per scan, strongest first.
     *
     * 2. The 96 node cap was applied in BSSID order -- the tree's own order
     *    -- and ic_tree holds ~200 nodes on this machine.  Every AP whose
     *    BSSID sorted past the cap was invisible no matter how strong it
     *    was.  The whole tree is published now.
     */
    struct ieee80211_node *list[512];
    RB_FOREACH(ni, ieee80211_tree, &ic->ic_tree) {
        if (nodes >= sizeof(list) / sizeof(list[0]))
            break;
        if (ni->ni_esslen > 32)
            continue;
        if (!ni->ni_chan || ni->ni_chan == IEEE80211_CHAN_ANYC)
            continue;
        if (IEEE80211_IS_CHAN_5GHZ(ni->ni_chan))
            ++n5g;
        else
            ++n2g;
        list[nodes++] = ni;
    }
    /* Insertion sort, strongest first; nodes <= 512 so O(n^2) is cheap. */
    for (unsigned i = 1; i < nodes; ++i) {
        struct ieee80211_node *key = list[i];
        unsigned j = i;
        while (j > 0 && list[j - 1]->ni_rssi < key->ni_rssi) {
            list[j] = list[j - 1];
            --j;
        }
        list[j] = key;
    }
    /*
     * PATCH [local-p27]: hand the panel a short list.  The tree holds 200+
     * candidates in a dense neighbourhood and the menu became unreadable
     * ("138 networks, can't even browse them").  Everything past the cap is
     * too weak to join anyway; the associated BSSID is pinned to the last
     * slot so airportd never loses sight of the current network.
     */
    const uint32_t wclPublishCap = 40;
    if (!wclPublishAll && nodes > wclPublishCap) {
        struct ieee80211_node *keep =
            (assocNotified && ic->ic_bss) ? ic->ic_bss : nullptr;
        if (keep) {
            for (unsigned i = wclPublishCap; i < nodes; ++i)
                if (list[i] == keep) {
                    list[wclPublishCap - 1] = keep;
                    break;
                }
        }
        nodes = wclPublishCap;
    }
    unsigned logged = 0, skipped = 0, sent = 0;
    /*
     * PATCH [local-p27]: the batch itself goes through publishWCLBeacon(),
     * whose live-post gate must not swallow it (it ran with scanPending
     * already false, which is exactly the state the gate blocks).  Latch a
     * flag for the duration of the loop.
     */
    wclBatchPublishing = true;
    for (unsigned i = 0; i < nodes; ++i) {
        /*
         * PATCH [local-p25]: hand over a beacon-shaped element list instead
         * of SSID + RSN only.  See wclBuildBeaconIEs().
         */
        uint8_t ies[320];
        size_t length = wclBuildBeaconIEs(ic, list[i], ies, sizeof(ies));
        if (length == 0) {
            ++skipped;
            continue;
        }
        if (logged < 4 && IEEE80211_IS_CHAN_5GHZ(list[i]->ni_chan)) {
            XYLog("p25 5g sample ch=%u rssi=%d ies=%zu rates=%u ht=%d vht=%d\n",
                ieee80211_chan2ieee(ic, list[i]->ni_chan),
                IWM_MIN_DBM + list[i]->ni_rssi, length,
                list[i]->ni_rates.rs_nrates,
                (list[i]->ni_flags & IEEE80211_NODE_HTCAP) != 0,
                (list[i]->ni_flags & IEEE80211_NODE_VHTCAP) != 0);
            ++logged;
        }
        publishWCLBeacon(list[i], ies, length);
        ++sent;
    }
    wclBatchPublishing = false;
    XYLog("publishWCLCachedResults nodes=%u cap=%u sent=%u 2g=%u 5g=%u "
        "skipped=%u maxnodes=%d\n", nodes, wclPublishAll ? 0 : wclPublishCap,
        sent, n2g, n5g, skipped, ic->ic_max_nnodes);
}

void AirportItlwmSkywalkInterface::publishWCLBeacon(const ieee80211_node *node, const uint8_t *ies, size_t length)
{
    if (!node || node->ni_esslen > 32 || !AirportWCL::validIEs(ies, length))
        return;
    ieee80211com *ic = fHalService->get80211Controller();
    if (!node->ni_chan || node->ni_chan == IEEE80211_CHAN_ANYC)
        return;
    unsigned channel = ieee80211_chan2ieee(ic, node->ni_chan);
    if (!channel || channel > 196)
        return;
    size_t size = sizeof(AirportWCL::BeaconMetadata) + length;
    auto metadata = static_cast<AirportWCL::BeaconMetadata *>(IOMalloc(size));
    if (!metadata)
        return;
    *metadata = {};
    metadata->ieLength = uint32_t(length);
    metadata->channelSpec = uint16_t(channel | (IEEE80211_IS_CHAN_5GHZ(node->ni_chan) ? 0xd000 : 0x1000));
    metadata->channel = channel;
    metadata->ssidLength = node->ni_esslen;
    memcpy(metadata->ssid, node->ni_essid, node->ni_esslen);
    memcpy(metadata->bssid, node->ni_bssid, 6);
    metadata->rssi = IWM_MIN_DBM + node->ni_rssi;
    metadata->interval = node->ni_intval;
    metadata->capability = node->ni_capinfo;
    metadata->flags = AirportWCL::SSIDPresent | AirportWCL::RSSIValid;
    memcpy(metadata + 1, ies, length);
    metadata->ieLength = uint32_t(AirportWCL::selectWPA2TransitionMode(
        reinterpret_cast<uint8_t *>(metadata + 1), length));
    size_t messageSize = sizeof(*metadata) + metadata->ieLength;
    if (assocNotified && IEEE80211_ADDR_EQ(node->ni_bssid, joinBSSID)) {
        wclBssValid = messageSize <= sizeof(wclBssInfo);
        if (wclBssValid) {
            bzero(wclBssInfo, sizeof(wclBssInfo));
            memcpy(wclBssInfo, metadata, messageSize);
        }
    }
    if (scanPending) {
        ++wclScanResults;
        /*
         * Quiet-period completion: the channel walk is done once no new
         * beacon has arrived for a while.  Bounded by wclScanDeadline so a
         * busy radio cannot defer the completion forever.
         */
        if (airport_up_time() < wclScanDeadline)
            scanSource->setTimeoutMS(1200);
        /*
         * PATCH [local-p23]: collect during the walk, publish afterwards.
         * Sending here meant one message per beacon in channel order, which
         * both kept rebuilding the menu mid-scan and put the strong 5 GHz
         * APs last.  finishWCLScan() now emits the sorted set in one burst.
         */
        IOFree(metadata, size);
        return;
    }
    /*
     * PATCH [local-p27]: live per-beacon ScanResult posts are the leak behind
     * "sometimes 40, sometimes the full list".  The driver capped its batch
     * at 40, yet airportd kept 170-190 cached entries -- fed by this path:
     * when unassociated the net80211 stack sits in S_SCAN rotating channels
     * with scanPending false, so every beacon fell through to a live,
     * uncapped post, and airportd merges without evicting.  The batch from
     * publishWCLCachedResults() (flagged by wclBatchPublishing) is now the
     * ONLY source of scan results.  "-mtk-allwifi" restores the legacy live
     * stream; the associated BSS stays exempt so the join flow keeps updates.
     */
    if (!wclBatchPublishing && !wclPublishAll &&
        !(assocNotified && IEEE80211_ADDR_EQ(node->ni_bssid, joinBSSID))) {
        IOFree(metadata, size);
        return;
    }
    instance->postMessage(this, AirportWCL::ScanResult, metadata, messageSize, true);
    IOFree(metadata, size);
}

void AirportItlwmSkywalkInterface::clearWCLPMKSA()
{
    auto ic = fHalService->get80211Controller();
    ieee80211_pmk *pmk;
    while ((pmk = TAILQ_FIRST(&ic->ic_pmksa))) {
        TAILQ_REMOVE(&ic->ic_pmksa, pmk, pmk_next);
        explicit_bzero(pmk, sizeof(*pmk));
        ::free(pmk);
    }
}

IOReturn AirportItlwmSkywalkInterface::setCLEAR_PMKSA_CACHE(void *)
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(11));
}

IOReturn AirportItlwmSkywalkInterface::installWCLKey(apple80211_key *key)
{
    static_assert(sizeof(apple80211_key) == 0x94, "WCL cipher key layout");
    if (!key || !AirportWCL::validCipherKey(key->key_cipher_type, key->key_len,
            key->key_flags, key->key_index, key->key_rsc_len))
        return kIOReturnBadArgument;
    auto ic = fHalService->get80211Controller();
    auto ni = ic->ic_bss;
    if (!ic->ic_wcl_enterprise || !ni || ic->ic_state != IEEE80211_S_RUN)
        return kIOReturnNotReady;
    if (key->key_cipher_type == APPLE80211_CIPHER_PMK ||
        key->key_cipher_type == APPLE80211_CIPHER_PMKSA ||
        key->key_cipher_type == APPLE80211_CIPHER_MSK) {
        // WCL's native user-client rejects PTK/GTK, and delivers a PMK/MSK
        // instead. Use the existing net80211 four-way/group-key handshake.
        if (!ieee80211_is_8021x_akm((enum ieee80211_akm)ni->ni_rsnakms))
            return kIOReturnBadArgument;
        uint8_t any = 0;
        for (auto byte : key->key_ea.octet) any |= byte;
        if (any && !IEEE80211_ADDR_EQ(key->key_ea.octet, ni->ni_bssid))
            return kIOReturnBadArgument;
        auto pmk = ieee80211_pmksa_add(ic, (enum ieee80211_akm)ni->ni_rsnakms,
                                     ni->ni_macaddr, key->key, 0);
        IOReturn result = pmk ? kIOReturnSuccess : kIOReturnNoMemory;
        return result;
    }
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmSkywalkInterface::accessWCLRSN(apple80211_rsn_ie_data *data, bool write)
{
    if (!data) return kIOReturnBadArgument;
    auto ic = fHalService->get80211Controller();
    if (write) {
        if (!AirportWCL::validRSNOverride(data->ie, data->len))
            return kIOReturnBadArgument;
        // net80211 constructs negotiated IEs for its own key handshake.
        return kIOReturnUnsupported;

    }
    const uint8_t *ie = ic->ic_bss ? ic->ic_bss->ni_rsnie : nullptr;
    if (!ie) return kIOReturnNotReady;
    size_t length = size_t(ie[1]) + 2;
    if (length > sizeof(data->ie)) return kIOReturnBadArgument;
    bzero(data, sizeof(*data));
    data->version = APPLE80211_VERSION;
    data->len = length;
    memcpy(data->ie, ie, length);
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::beginWCLJoin(apple80211AssocCandidates *request)
{
    AirportWCL::JoinRequest join;
    if (!AirportWCL::decodeJoin(request, 0x2cc, join)) {
        return kIOReturnBadArgument;
    }
    return startWCLJoin(join);
}

IOReturn AirportItlwmSkywalkInterface::startWCLJoin(AirportWCL::JoinRequest &join)
{
    auto auth = AirportWCL::classifyLegacyAuth(join);
    XYLog("WCL join ssid_len=%u bssid=%s chanspec=0x%x auth lower=%u upper=0x%x -> %s, key len=%u type=%u%s, rsn_ie len=%u\n",
          join.ssidLength, ether_sprintf(join.bssid), join.channelSpec, join.lowerAuth, join.upperAuth,
          AirportWCL::legacyAuthName(auth), join.keyLength, join.keyCipher,
          AirportWCL::isPassphrase(join) ? " (passphrase)" : "", join.rsnLength);
    bool personal = auth == AirportWCL::LegacyAuth::Personal;
    bool enterprise = auth == AirportWCL::LegacyAuth::Enterprise;
    bool supported = auth != AirportWCL::LegacyAuth::Invalid;
    ieee80211com *ic = fHalService->get80211Controller();
    // Applying a private MAC restarts firmware asynchronously. WCL can issue
    // the association before that restart has completed; keep one request
    // until the HAL reports READY rather than making the user retry it.
    if (supported && !joinPending && ic->ic_wcl_mac_reconfig &&
        (ic->ic_if.if_flags & IFF_UP)) {
        deferredJoin = join;
        memcpy(joinBSSID, join.bssid, sizeof(joinBSSID));
        explicit_bzero(&join, sizeof(join));
        joinDeferred = joinPending = true;
        joinTimer->setTimeoutMS(30000);
        return kIOReturnSuccess;
    }
    if (!supported || !(ic->ic_if.if_flags & IFF_RUNNING) || joinPending) {
        explicit_bzero(&join, sizeof(join));
        return !supported ? kIOReturnUnsupported : kIOReturnBusy;
    }
    /*
     * PATCH [local-p15]: a live walk is only a few seconds of channel
     * hopping.  Answering kIOReturnBusy makes airportd drop the request,
     * issue leaveNetwork and retry - which tears down the link we are about
     * to build and costs a full join-timeout cycle (16.1 s measured).
     * Hold the request instead and replay it the moment the walk finishes
     * (finishWCLScan -> resumeWCLJoin); joinTimer stays armed as the backstop.
     */
    if (scanPending) {
        deferredJoin = join;
        memcpy(joinBSSID, join.bssid, sizeof(joinBSSID));
        explicit_bzero(&join, sizeof(join));
        joinDeferred = joinPending = true;
        assocNotified = firstBeaconNotified = portAuthorized = false;
        joinTimer->setTimeoutMS(30000);
        XYLog("WCL join: deferred behind running scan (state=%d)\n", ic->ic_state);
        return kIOReturnSuccess;
    }
    if (ic->ic_state == IEEE80211_S_RUN || ic->ic_state == IEEE80211_S_AUTH || ic->ic_state == IEEE80211_S_ASSOC) {
        XYLog("WCL leave: new join preempts state=%d\n", ic->ic_state);
        leaveWCLNetwork();
    }
    ieee80211_disable_rsn(ic);
    ieee80211_disable_wep(ic);
    /* PATCH [local-p18]: pre-join wipe; the new key lands below and clears it */
    ic->ic_wcl_psk_wiped = true;
    explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
    clearWCLPMKSA();
    ic->ic_flags &= ~(IEEE80211_F_AUTO_JOIN | IEEE80211_F_PSK);
    ic->ic_wcl_enterprise = enterprise;

    if (personal || enterprise) {
        ieee80211_wpaparams wpa = {};
        wpa.i_enabled = 1;
        if (join.upperAuth & (APPLE80211_AUTHTYPE_WPA | APPLE80211_AUTHTYPE_WPA_PSK))
            wpa.i_protos |= IEEE80211_WPA_PROTO_WPA1;
        if (join.upperAuth & (APPLE80211_AUTHTYPE_WPA2 | APPLE80211_AUTHTYPE_WPA2_PSK |
                              APPLE80211_AUTHTYPE_SHA256_PSK | APPLE80211_AUTHTYPE_SHA256_8021X))
            wpa.i_protos |= IEEE80211_WPA_PROTO_WPA2;
        if (join.upperAuth & (APPLE80211_AUTHTYPE_WPA_PSK | APPLE80211_AUTHTYPE_WPA2_PSK))
            wpa.i_akms |= IEEE80211_WPA_AKM_PSK;
        if (join.upperAuth & (APPLE80211_AUTHTYPE_WPA | APPLE80211_AUTHTYPE_WPA2))
            wpa.i_akms |= IEEE80211_WPA_AKM_8021X;
        if (join.upperAuth & APPLE80211_AUTHTYPE_SHA256_PSK)
            wpa.i_akms |= IEEE80211_WPA_AKM_SHA256_PSK;
        if (join.upperAuth & APPLE80211_AUTHTYPE_SHA256_8021X)
            wpa.i_akms |= IEEE80211_WPA_AKM_SHA256_8021X;
        wpa.i_ciphers = IEEE80211_WPA_CIPHER_CCMP | IEEE80211_WPA_CIPHER_TKIP;
        if (join.rsnLength >= 8 && join.rsn[0] == IEEE80211_ELEMID_RSN &&
            join.rsn[4] == 0 && join.rsn[5] == 0x0f && join.rsn[6] == 0xac) {
            if (join.rsn[7] == 2) wpa.i_groupcipher = IEEE80211_WPA_CIPHER_TKIP;
            if (join.rsn[7] == 4) wpa.i_groupcipher = IEEE80211_WPA_CIPHER_CCMP;
        }
        int error = ieee80211_ioctl_setwpaparms(ic, &wpa);
        if (error && error != ENETRESET) {
            explicit_bzero(&join, sizeof(join));
            return kIOReturnBadArgument;
        }
        if (personal) {
            if (AirportWCL::isPassphrase(join)) {
                char passphrase[33] = {};
                memcpy(passphrase, join.key, join.keyLength);
                pbkdf2_sha1(passphrase, join.ssid, join.ssidLength, 4096,
                            ic->ic_psk, sizeof(ic->ic_psk));
                explicit_bzero(passphrase, sizeof(passphrase));
            } else
                memcpy(ic->ic_psk, join.key, sizeof(ic->ic_psk));
            ic->ic_flags |= IEEE80211_F_PSK;
            XYLog("WCL join PSK key check %02x\n",
                  ieee80211_key_check(ic->ic_psk, sizeof(ic->ic_psk)));
        } else if (join.keyLength == IEEE80211_PMK_LEN) {
            bool cached = true;
            if (join.upperAuth & (APPLE80211_AUTHTYPE_WPA | APPLE80211_AUTHTYPE_WPA2))
                cached = ieee80211_pmksa_add(ic, IEEE80211_AKM_8021X, join.bssid, join.key, 0) != nullptr;
            if (cached && (join.upperAuth & APPLE80211_AUTHTYPE_SHA256_8021X))
                cached = ieee80211_pmksa_add(ic, IEEE80211_AKM_SHA256_8021X, join.bssid, join.key, 0) != nullptr;
            if (!cached) {
                clearWCLPMKSA();
                explicit_bzero(&join, sizeof(join));
                return kIOReturnNoMemory;
            }
        }
    } else if (auth == AirportWCL::LegacyAuth::Wep) {
        ieee80211_nwkey keys = {};
        keys.i_wepon = IEEE80211_NWKEY_WEP;
        keys.i_defkid = join.keyIndex + 1;
        keys.i_key[join.keyIndex].i_keylen = join.keyLength;
        keys.i_key[join.keyIndex].i_keydat = join.key;
        int error = ieee80211_ioctl_setnwkeys(ic, &keys);
        if (error && error != ENETRESET) {
            explicit_bzero(&join, sizeof(join));
            return kIOReturnBadArgument;
        }
    }
    ic->ic_des_esslen = join.ssidLength;
    memcpy(ic->ic_des_essid, join.ssid, sizeof(ic->ic_des_essid));
    memcpy(ic->ic_des_bssid, join.bssid, 6);
    memcpy(joinBSSID, join.bssid, 6);
    ic->ic_flags |= IEEE80211_F_DESBSSID;
    ic->ic_wcl_join_requested = true;
    ic->ic_wcl_scan_requested = false;
    /* PATCH [local-p18]: a fresh join supersedes any earlier wipe */
    ic->ic_wcl_psk_wiped = false;
    current_authtype_lower = join.lowerAuth;
    current_authtype_upper = join.upperAuth;
    explicit_bzero(&join, sizeof(join));
    wclBssValid = false;
    joinPending = true;
    assocNotified = firstBeaconNotified = portAuthorized = false;
    wclAssociatedBeacons = 0;
    bzero(wclBeaconInfo, sizeof(wclBeaconInfo));
    disassocIsVoluntary = false;
    joinTimer->setTimeoutMS(30000);
    if (ic->ic_state != IEEE80211_S_SCAN && ic->ic_newstate(ic, IEEE80211_S_SCAN, -1)) {
        joinPending = false;
        joinTimer->cancelTimeout();
        ic->ic_wcl_join_requested = false;
        clearWCLPMKSA();
        ic->ic_wcl_enterprise = false;
        /* PATCH [local-p18]: deliberate wipe, keep the zero-PSK log quiet */
        ic->ic_wcl_psk_wiped = true;
        explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
        return kIOReturnError;
    }
    /*
     * PATCH [local-p15]: airportd hands us a BSSID it just picked out of its
     * own scan results, so that node is normally already in ic_tree.  Walking
     * every channel again just to find it costs the whole sweep (measured
     * 5.4 s for 41 channels) and delays the first AUTH frame by exactly that
     * much; combined with the 15 s join timeout it produces the bimodal
     * 1.7 s / 16.1 s distribution seen on 2026-11-09.  Join the cached node
     * directly and fall back to the walk only when the cache really misses.
     */
    {
        struct ieee80211_node *cached = NULL;
        if (ic->ic_bss != NULL && ic->ic_des_esslen != 0) {
            struct ieee80211_node *ni = ieee80211_find_node(ic, joinBSSID);
            /*
             * PATCH [local-p17]: an entry can still be linked in ic_tree
             * after leaveWCLNetwork() cleared its contents, in which case
             * find_node() hands back a husk with ni_rsnprotos / akms /
             * ciphers zeroed.  Joining it makes node_join_bss() copy an
             * empty macaddr into ic_bss, and the AUTH retries then go to
             * ff:ff:ff:ff:ff:ff for six rounds until the watchdog resets
             * the state machine (measured 471.890: protos=0 akms=0
             * ciphers=0, then "AUTH: pre-auth deauth sent to
             * ff:ff:ff:ff:ff:ff", 20.9 s wasted).  Require a fully
             * populated RSN node when RSN is on, and hold a reference for
             * the duration of the join so the tree cannot free it
             * underneath us.
             */
            if (ni != NULL && ni->ni_chan != NULL && ni->ni_rssi != 0 &&
                (!(ic->ic_flags & IEEE80211_F_RSNON) ||
                 ni->ni_rsnprotos != 0) &&
                ni->ni_esslen == ic->ic_des_esslen &&
                memcmp(ni->ni_essid, ic->ic_des_essid, ni->ni_esslen) == 0)
                cached = ieee80211_ref_node(ni);
            else if (ni != NULL)
                XYLog("WCL join: cached node unusable "
                      "protos=%d akms=%d ciphers=%d, scan instead\n",
                      ni->ni_rsnprotos, ni->ni_rsnakms, ni->ni_rsnciphers);
        }
        if (cached != NULL) {
            XYLog("WCL join: cached node hit rssi=%d chan=%d, skip scan\n",
                  cached->ni_rssi, ieee80211_chan2ieee(ic, cached->ni_chan));
            /*
             * We are bypassing ieee80211_end_scan(), which is what normally
             * clears the scan-in-progress flags.  Left behind they make the
             * watchdog think a walk is still live.  Dropping F_BGSCAN here is
             * safe for the same reason the self-heal in beginWCLScan() does it:
             * a cache scan cannot be meaningful once we are at or below S_SCAN.
             */
            ic->ic_wcl_scan_idle = false;
            ic->ic_flags &= ~(IEEE80211_F_ASCAN | IEEE80211_F_BGSCAN);
            ieee80211_node_join_bss(ic, cached);
            ieee80211_free_node(ic, cached);
        } else if (ic->ic_wcl_scan_idle) {
            // The stack waits for us between scans (ieee80211_end_scan).
            ic->ic_wcl_scan_idle = false;
            ieee80211_next_scan(&ic->ic_if);
        }
    }
    return kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::resumeWCLJoin()
{
    if (!joinDeferred || wclStopping)
        return;
    AirportWCL::JoinRequest join = deferredJoin;
    explicit_bzero(&deferredJoin, sizeof(deferredJoin));
    joinDeferred = joinPending = false;
    joinTimer->cancelTimeout();
    IOReturn status = startWCLJoin(join);
    explicit_bzero(&join, sizeof(join));
    if (status != kIOReturnSuccess) {
        joinPending = true;
        finishWCLJoin(1, 0, status);
    }
}

void AirportItlwmSkywalkInterface::finishWCLJoin(uint16_t status, uint16_t reason, IOReturn driverStatus)
{
    if (!joinPending)
        return;
    /* PATCH [local-p11]: caller trace, so a bad round can be attributed */
    XYLog("WCL join complete status=%u reason=%u driver=%d state=%d\n",
          status, reason, (int)driverStatus,
          fHalService->get80211Controller()->ic_state);
    AirportWCL::ConnectionEvent event = {};
    event.status = status;
    event.reason = reason;
    memcpy(event.peers[0].bssid, joinBSSID, 6);
    event.peers[0].driverStatus = driverStatus;
    joinPending = false;
    joinDeferred = false;
    explicit_bzero(&deferredJoin, sizeof(deferredJoin));
    joinTimer->cancelTimeout();
    instance->postMessage(this, AirportWCL::ConnectComplete, &event, sizeof(event), true);
    if (status) {
        portAuthorized = false;
        updateWCLLinkQuality(true);
        ieee80211com *ic = fHalService->get80211Controller();
        ic->ic_wcl_join_requested = false;
        clearWCLPMKSA();
        ic->ic_wcl_enterprise = false;
        /*
         * PATCH [local-p18]: flag the wipe.  The zero-PSK guard in
         * ieee80211_input.c cannot tell "we wiped it on purpose" from
         * "the key vanished and the state machine is still running";
         * without this bit every failed join produced a second, bogus
         * "with no PMK ... re-auth cannot succeed" report (measured
         * 8 bogus reports against 8 failed joins in the 5-round P17
         * run) and buried the real signal.
         */
        ic->ic_wcl_psk_wiped = true;
        explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
        /*
         * PATCH [local-p14]: the join is over, so stop the state machine
         * too.  Wiping ic_psk here while leaving the driver in AUTH or
         * ASSOC is what produced the "psk check 80" loop: debugPoll kept
         * re-sending AUTH_REQ / ASSOC_REQ, the AP kept accepting them,
         * and every 4-way ran on 32 zero bytes.  Parking in S_INIT also
         * lets beginWCLScan() work again, which is what the reconnect
         * needs in the first place.
         */
        if (ic->ic_state > IEEE80211_S_SCAN) {
            XYLog("WCL join failed, parking in S_INIT (was state=%d)\n",
                  ic->ic_state);
            ieee80211_new_state(ic, IEEE80211_S_INIT, -1);
        }
    } else {
        /*
         * PATCH [local-p28]: end the join session on success too.  Leaving
         * ic_wcl_join_requested set after a completed join let the net80211
         * state machine re-match the stale desired ESS out of the node tree
         * after any power cycle or beacon loss -- an association airportd
         * never asked for and never learned about ("zombie"): its UI showed
         * disconnected, its live scans got rejected busy and every join by
         * name failed with "Could not find network".  macOS owns joins; a
         * reconnect after a drop must come from airportd as a fresh request.
         */
        fHalService->get80211Controller()->ic_wcl_join_requested = false;
        /* PATCH [local-p18]: the key is live again, so a later zero-PSK
         * report is a genuine fault and must not be suppressed. */
        fHalService->get80211Controller()->ic_wcl_psk_wiped = false;
    }
}

void AirportItlwmSkywalkInterface::wclJoinTimeout(OSObject *owner, IOTimerEventSource *)
{
    auto self = OSDynamicCast(AirportItlwmSkywalkInterface, owner);
    if (self && !self->wclStopping) {
        XYLog("WCL leave: join timeout fired (state=%d)\n",
              self->fHalService->get80211Controller()->ic_state);
        self->finishWCLJoin(1, 1004, kIOReturnTimeout);
        self->leaveWCLNetwork();
    }
}

IOReturn AirportItlwmSkywalkInterface::leaveWCLNetwork()
{
    ieee80211com *ic = fHalService->get80211Controller();
    portAuthorized = false;
    updateWCLLinkQuality(true);
    joinPending = false;
    joinDeferred = false;
    explicit_bzero(&deferredJoin, sizeof(deferredJoin));
    joinTimer->cancelTimeout();
    ic->ic_wcl_join_requested = false;
    wclAssociatedBeacons = 0;
    wclBssValid = false;
    disassocIsVoluntary = true;
    ic->ic_deauth_reason = IEEE80211_REASON_AUTH_LEAVE;
    if (ic->ic_state > IEEE80211_S_SCAN)
        ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
    ieee80211_disable_rsn(ic);
    ieee80211_disable_wep(ic);
    /* PATCH [local-p18]: deliberate wipe on leave, not a fault */
    ic->ic_wcl_psk_wiped = true;
    explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
    clearWCLPMKSA();
    ic->ic_wcl_enterprise = false;
    ic->ic_flags &= ~(IEEE80211_F_PSK | IEEE80211_F_DESBSSID);
    ic->ic_des_esslen = 0;
    bzero(ic->ic_des_essid, sizeof(ic->ic_des_essid));
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::abortWCLJoin()
{
    XYLog("WCL leave: abort join request from airportd (state=%d joinPending=%d)\n",
          fHalService->get80211Controller()->ic_state, (int)joinPending);
    leaveWCLNetwork();
    instance->postMessage(this, AirportWCL::JoinAbortComplete, nullptr, 0, true);
    return kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::handleWCLLink(bool up)
{
    if (wclStopping)
        return;
    portAuthorized = up;
    if (!up && wclLinkIndicated) {
        AirportWCL::LinkStatusEvent event = {};
        memcpy(event.bssid, joinBSSID, sizeof(event.bssid));
        event.reason = 255;
        wclLinkIndicated = false;
        wclBssValid = false;
        instance->postMessage(this, AirportWCL::LinkStatus, &event, sizeof(event), true);
    }
    if (up && joinPending && firstBeaconNotified)
        finishWCLJoin(0, 0, kIOReturnSuccess);
    updateWCLLinkQuality(true);
}

void AirportItlwmSkywalkInterface::handleWCLEvent(int code, void *data)
{
    if (wclStopping)
        return;
    // Firmware initialization runs on systq, outside the native PostOffice gate.
    // Serialize only lifecycle notifications; never run synchronous WCL ioctls here.
    if ((code == IEEE80211_EVT_DRIVER_RESET_BEGIN || code == IEEE80211_EVT_DRIVER_READY ||
         code == IEEE80211_EVT_DRIVER_STOPPED) && !instance->getWorkQueue()->inGate()) {
        _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(12),
                                reinterpret_cast<void *>(uintptr_t(code)));
        return;
    }
    ieee80211com *ic = fHalService->get80211Controller();
    switch (code) {
        case IEEE80211_EVT_COUNTRY_CODE_UPDATE:
            deferWCLCountryUpdate();
            break;
        case IEEE80211_EVT_DRIVER_RESET_BEGIN: {
            portAuthorized = false;
            updateWCLLinkQuality(true);
            if (ic->ic_wcl_mac_reconfig) {
                break;
            }
            uint32_t reset = 0; // Native resetInterfacesBegin.
            instance->postMessage(this, AirportWCL::ResetInterface, &reset, sizeof(reset), true);
            break;
        }
        case IEEE80211_EVT_DRIVER_READY:
        case IEEE80211_EVT_DRIVER_STOPPED: {
            bool ready = code == IEEE80211_EVT_DRIVER_READY;
            if (!ready) {
                finishWCLScan(ic->ic_wcl_mac_reconfig ? kIOReturnAborted : kIOReturnOffline);
                finishWCLJoin(1, 0, kIOReturnOffline);
                portAuthorized = false;
                updateWCLLinkQuality(true);
                ic->ic_wcl_join_requested = false;
                clearWCLPMKSA();
                ic->ic_wcl_enterprise = false;
                explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
            }
            // An address application is requested by WCL itself, not a new
            // adapter reset. Replaying firmware-load events here rotates the
            // scan MAC again and recursively restarts the card.
            if (AirportWCL::consumeMacReconfiguration(ic->ic_wcl_mac_reconfig, ready)) {
                if (ready)
                    resumeWCLJoin();
                break;
            }
            if (ready) {
                uint32_t reset = 1; // Native resetInterfacesComplete, only after HAL success.
                instance->postMessage(this, AirportWCL::ResetInterface, &reset, sizeof(reset), true);
            }
            AirportWCL::DriverAvailability event = {};
            event.version = 3;
            event.available = ready;
            event.reason = ready ? 0xe0821803 : 0xe0821804;
            instance->postMessage(this, AirportWCL::DriverAvailable, &event, sizeof(event), true);
            if (ready)
                resumeWCLJoin();
            break;
        }
        case IEEE80211_EVT_BEACON: {
            auto beacon = static_cast<ieee80211_beacon_event *>(data);
            if (!beacon)
                break;
            // Retain the actual associated beacon, including its advancing TSF.
            // WCL polls BEACON_INFO to distinguish fresh reception from a stale BSS.
            if (assocNotified && (joinPending || portAuthorized) &&
                IEEE80211_ADDR_EQ(beacon->node->ni_bssid, joinBSSID) &&
                beacon->frame && beacon->frame_length >= 36 &&
                beacon->frame_length <= 0x800 &&
                (beacon->frame[0] & 0xfc) == IEEE80211_FC0_SUBTYPE_BEACON) {
                uint32_t length = uint32_t(beacon->frame_length);
                bzero(wclBeaconInfo, sizeof(wclBeaconInfo));
                memcpy(wclBeaconInfo, &length, sizeof(length));
                memcpy(wclBeaconInfo + 4, beacon->frame, length);
                ++wclAssociatedBeacons;
                ++wclLqmBeacons;
                // WCL has no legacy LQMData instance at createLQMData().
                // Send the snapshot consumed by WCL instead of legacy 227/229.
                if (portAuthorized && (wclLqmBeacons % 8 == 0)) {
#if __IO80211_TARGET == __MAC_15_2
                    apple80211_rssi_data rssi = {};
                    if (getRSSI(&rssi) == kIOReturnSuccess) {
                        AirportWCL::SequoiaLqmUpdate update = {};
                        update.rssiValid = 1;
                        update.rssi = rssi.aggregate_rssi;
                        instance->postMessage(this, AirportWCL::LqmUpdate,
                            &update, sizeof(update), true);
                    }
#else
                    AirportWCL::LqmBeaconUpdate update;
                    if (AirportWCL::makeBeaconUpdate(
                            wclLqmBeacons - wclLqmReportedBeacons, update)) {
                        instance->postMessage(this, AirportWCL::LqmUpdate,
                            &update, sizeof(update), true);
                        wclLqmReportedBeacons = wclLqmBeacons;
                    }
#endif
                    updateWCLLinkQuality();
                }
            }
            if (scanPending || joinPending || ic->ic_state == IEEE80211_S_SCAN ||
                (portAuthorized && IEEE80211_ADDR_EQ(beacon->node->ni_bssid, joinBSSID)))
                publishWCLBeacon(beacon->node, beacon->ies, beacon->length);
            if (joinPending && assocNotified && !firstBeaconNotified &&
                IEEE80211_ADDR_EQ(beacon->node->ni_bssid, joinBSSID)) {
                AirportWCL::FirstBeaconEvent event = {};
                memcpy(event.peer.bssid, joinBSSID, 6);
                if (!wclBssValid)
                    break;
                firstBeaconNotified = true;
                AirportWCL::LinkStatusEvent link = {};
                memcpy(link.bssid, joinBSSID, sizeof(link.bssid));
                link.up = 1;
                link.reason = 255;
                wclLinkIndicated = true;
                instance->postMessage(this, AirportWCL::LinkStatus, &link, sizeof(link), true);
                instance->postMessage(this, AirportWCL::FirstBeacon, &event, sizeof(event), true);
                if (portAuthorized)
                    finishWCLJoin(0, 0, kIOReturnSuccess);
            }
            break;
        }
        case IEEE80211_EVT_SCAN_DONE:
            if (scanPending) {
                if (scanSkipCompletion)
                    scanSkipCompletion = false;
                else {
                    scanCompleted = true;
                    scanSource->setTimeoutMS(0);
                }
            }
            break;
        case IEEE80211_EVT_STA_ASSOC_DONE:
            if (joinPending && !assocNotified) {
                AirportWCL::AssociationEvent event = {};
                event.status = ic->ic_assoc_status;
                memcpy(event.bssid, joinBSSID, 6);
                event.driverStatus = event.status ? kIOReturnError : kIOReturnSuccess;
                assocNotified = !event.status;
                instance->postMessage(this, AirportWCL::AssocComplete, &event, sizeof(event), true);
                if (event.status)
                    finishWCLJoin(event.status, 0, kIOReturnError);
            }
            break;
        case IEEE80211_EVT_STA_DEAUTH: {
            portAuthorized = false;
            updateWCLLinkQuality(true);
            AirportWCL::DeauthEvent event = {};
            event.reason = ic->ic_deauth_reason;
            if (ic->ic_bss)
                memcpy(event.bssid, ic->ic_bss->ni_bssid, 6);
            instance->postMessage(this, AirportWCL::Deauth, &event, sizeof(event), true);
            finishWCLJoin(1, event.reason, kIOReturnOffline);
            break;
        }
        default: break;
    }
}

IOReturn AirportItlwmSkywalkInterface::getWCL_VALID_CHANNEL_COUNT(unsigned long *count)
{
    if (!count)
        return kIOReturnBadArgument;
    *count = 0;
    ieee80211com *ic = fHalService->get80211Controller();
    for (unsigned i = 1; i < IEEE80211_CHAN_MAX; ++i)
        if (isset(ic->ic_chan_active, i))
            ++*count;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_LINK_STATE_UPDATE(apple80211_wcl_update_link_state *data)
{
    if (!data)
        return kIOReturnBadArgument;
#if __IO80211_TARGET == __MAC_15_2
    const auto bytes = reinterpret_cast<const uint8_t *>(data);
    if (bytes[6]) {
        if (bytes[8])
            setCurrentApAddress(reinterpret_cast<ether_addr *>(data));
        if (bytes[7])
            setLinkState(kIO80211NetworkLinkUp, 0, false, 0);
    } else {
        uint32_t debounce = AirportWCL::read32(bytes + 12);
        setCurrentApAddress(nullptr);
        setLinkState(kIO80211NetworkLinkDown, 0, debounce != 0, debounce);
    }
    IOReturn status = kIOReturnSuccess;
#else
    IOReturn status = IO80211InfraInterface::setWCL_LINK_STATE_UPDATE(data);
#endif
    if (status == kIOReturnSuccess)
        updateWCLLinkQuality(true);
    return status;
}

IOReturn AirportItlwmSkywalkInterface::getWCL_BSS_INFO(apple80211_beacon_msg *data)
{
    if (!data)
        return kIOReturnBadArgument;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(5), data);
}

IOReturn AirportItlwmSkywalkInterface::getWCL_EXTENDED_BSS_INFO(apple80211_extended_bss_info *data)
{
    if (!data)
        return kIOReturnBadArgument;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(6), data);
}

IOReturn AirportItlwmSkywalkInterface::copyWCLExtendedBss(void *data)
{
    ieee80211com *ic = fHalService->get80211Controller();
    if (!data || !ic->ic_bss || ic->ic_state != IEEE80211_S_RUN)
        return kIOReturnNotReady;
    static_assert(sizeof(apple80211_rate_set_data) == 0xbc, "Extended BSS rates size");
    static_assert(sizeof(apple80211_mcs_index_set_data) == 0x10, "Extended BSS MCS size");
    auto bytes = static_cast<uint8_t *>(data);
#if __IO80211_TARGET == __MAC_15_2
    bzero(bytes, 0x1e0);
#else
    bzero(bytes, 0x214);
#endif
    IOReturn status = getRATE_SET(reinterpret_cast<apple80211_rate_set_data *>(bytes));
    if (status)
        return status;
    status = getMCS_INDEX_SET(reinterpret_cast<apple80211_mcs_index_set_data *>(bytes + 0xbc));
    if (status)
        return status;
    apple80211_vht_mcs_index_set_data vht = {};
    vht.version = APPLE80211_VERSION;
    vht.mcs_map = 0xffff;
    if (ic->ic_bss->ni_flags & IEEE80211_NODE_VHT)
        vht.mcs_map = ic->ic_bss->ni_vht_mcsinfo.tx_mcs_map;
    memcpy(bytes + 0xcc, &vht, sizeof(vht));
    // Native updateMCSSet uses version + a 16-bit HE MCS/NSS map (8 bytes).
    uint32_t heVersion = APPLE80211_VERSION;
    uint16_t heMap = 0xffff;
    if (ic->ic_bss->ni_flags & IEEE80211_NODE_HE) {
        bool wide = ic->ic_bss->ni_chw == IEEE80211_CHAN_WIDTH_160;
        heMap = htole16(ieee80211_he_mcs_intersection(
            le16toh(wide ? ic->ic_he_mcs_nss_supp.rx_mcs_160 : ic->ic_he_mcs_nss_supp.rx_mcs_80),
            le16toh(wide ? ic->ic_bss->ni_he_mcs_nss_supp.tx_mcs_160 : ic->ic_bss->ni_he_mcs_nss_supp.tx_mcs_80)));
    }
    memcpy(bytes + 0xd4, &heVersion, sizeof(heVersion));
    memcpy(bytes + 0xd8, &heMap, sizeof(heMap));
    if (ic->ic_flags & IEEE80211_F_RSNON)
#if __IO80211_TARGET == __MAC_15_2
        ieee80211_add_rsn(bytes + 0xdd, ic, ic->ic_bss);
#else
        ieee80211_add_rsn(bytes + 0x113, ic, ic->ic_bss);
#endif
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_UPDATE_FAST_LANE(apple80211_fastlane *data)
{
    if (!data)
        return kIOReturnBadArgument;
    const auto bytes = reinterpret_cast<const uint8_t *>(data);
    return (bytes[0] || bytes[1]) ? kIOReturnUnsupported : kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::getWCL_LOW_LATENCY_INFO(apple80211_low_latency_info *data)
{
    if (!data)
        return kIOReturnBadArgument;
    // No low-latency or P2P session is implemented by this adapter.
    bzero(data, 4);
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::getWCL_CHANNELS_INFO(apple80211ChannelInfo *data)
{
    if (!data)
        return kIOReturnBadArgument;
    auto info = reinterpret_cast<AirportWCL::ChannelInfo *>(data);
    bzero(info, sizeof(*info));
    ieee80211com *ic = fHalService->get80211Controller();
    for (unsigned i = 1; i < IEEE80211_CHAN_MAX && info->count < 400; ++i) {
        if (!isset(ic->ic_chan_active, i))
            continue;
        const ieee80211_channel *channel = &ic->ic_channels[i];
        auto &entry = info->channels[info->count++];
        entry.channel = i;
        entry.channelSpec = uint16_t(i | (IEEE80211_IS_CHAN_5GHZ(channel) ? 0xd000 : 0x1000));
        if (channel->ic_flags & IEEE80211_CHAN_DFS)
            entry.flags |= 2;
        if (channel->ic_flags & IEEE80211_CHAN_PASSIVE)
            entry.flags |= 4;
    }
    apple80211_country_code_data country = {};
    if (instance->getCOUNTRY_CODE(static_cast<OSObject *>(this), &country) == kIOReturnSuccess)
        memcpy(info->country, country.cc, 3);
    return info->count ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn AirportItlwmSkywalkInterface::getCHANNELS_INFO(apple80211_channels_info *data)
{
    if (!data)
        return kIOReturnBadArgument;
    if (!_fCommandGate)
        return kIOReturnNotReady;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(13), data);
}

IOReturn AirportItlwmSkywalkInterface::copyWCLChannels(void *data)
{
    auto info = static_cast<AirportWCL::NativeChannelInfo *>(data);
    bzero(info, sizeof(*info));
    ieee80211com *ic = fHalService->get80211Controller();
    for (unsigned i = 1; i <= IEEE80211_CHAN_MAX; ++i) {
        const ieee80211_channel *channel = &ic->ic_channels[i];
        if (!isset(ic->ic_chan_active, i) || !channel->ic_freq)
            continue;
        AirportWCL::appendChannel(*info, i, IEEE80211_IS_CHAN_5GHZ(channel),
            channel->ic_flags & IEEE80211_CHAN_DFS,
            channel->ic_flags & IEEE80211_CHAN_PASSIVE,
            IEEE80211_IS_CHAN_HT40(channel) || IEEE80211_IS_CHAN_VHT40(channel),
            IEEE80211_IS_CHAN_VHT80(channel));
    }
    apple80211_country_code_data country = {};
    if (instance->getCOUNTRY_CODE(static_cast<OSObject *>(this), &country) == kIOReturnSuccess)
        memcpy(info->country, country.cc, sizeof(country.cc));
    return info->count ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn AirportItlwmSkywalkInterface::getWCL_TRAFFIC_COUNTERS(apple80211_wcl_traffic_counters *data)
{
    if (!data)
        return kIOReturnBadArgument;
    if (!_fCommandGate)
        return kIOReturnNotReady;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(14), data);
}

IOReturn AirportItlwmSkywalkInterface::copyWCLTrafficCounters(void *data)
{
    ieee80211com *ic = fHalService->get80211Controller();
    uint64_t timestamp;
    absolutetime_to_nanoseconds(mach_absolute_time(), &timestamp);
    const auto counters = AirportWCL::makeTrafficCounters(
        ic->ic_wcl_tx_packets, ic->ic_wcl_rx_packets, timestamp);
    memcpy(data, &counters, sizeof(counters));
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_SET_SCAN_HOME_AWAY_TIME(scanHomeAndAwayTime *data)
{
    if (!data)
        return kIOReturnBadArgument;
    if (!_fCommandGate)
        return kIOReturnNotReady;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(15), data);
}
#endif

#ifdef AIRPORT_WCL
IOReturn AirportItlwmSkywalkInterface::getBEACON_INFO(apple80211_beacon_info_t *data)
{
    if (!_fCommandGate || !data)
        return kIOReturnNotReady;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(7), data);
}
#endif

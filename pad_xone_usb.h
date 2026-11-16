// SPDX-License-Identifier: MIT
// pad_xone_usb.h - the original Xbox One controller (2013, micro-USB, USB id 045e:02d1) read straight over USB, for the Mac front end (front_mac.m).
//
// GameController does not list this model, so macOS never starts it: the pad flashes and switches itself off after a minute. It speaks Microsoft's GIP protocol on
// interface 0 (vendor class) and waits for a "power on" packet on its interrupt OUT endpoint; after that it sends 18-byte input reports (type 0x20) on the interrupt IN endpoint.
// Report layout (from Linux's xpad driver; check there if a button is off):
//   byte 4: bit 2 Menu, bit 3 View, bit 4 A, bit 5 B, bit 6 X, bit 7 Y
//   byte 5: bit 0 up, 1 down, 2 left, 3 right, 4 LB, 5 RB, 6 L3, 7 R3
//   bytes 6-7 left trigger, 8-9 right trigger (0..1023, little endian)
//   bytes 10-17: left X, left Y, right X, right Y (int16, little endian, up is positive)
// The pad holds a player slot of its own (pad_slots.h) and feeds dm_pad_set_player like a GameController pad, with front_mac.m's pad_read mapping (buttons by position).
// Everything runs on the main queue, as GameController's handlers do.
// Link with: -framework IOUSBHost -framework IOKit. DM_XONE_USB=0 turns it off. Quit Steam first: it holds the pad and the interface cannot be opened twice.
#pragma once
#import <Foundation/Foundation.h>
#import <IOUSBHost/IOUSBHost.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <math.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include "pad_slots.h"

#ifndef LOG
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

#define XONE_VID 0x045e
#define XONE_PID 0x02d1
#define XONE_NAME "Xbox One Controller (USB)"
#define XONE_EP_IN 0x81    // interrupt IN of interface 0
#define XONE_EP_OUT 0x01   // interrupt OUT of interface 0

static pad_slots *x_slots;
static const int *x_merge;
static int (*x_input_on)(void);
static IONotificationPortRef x_port;
static io_iterator_t x_dev_iter;
static IOUSBHostDevice *x_dev;   // the pad itself: kept open so that its configuration (and with it the interface) stays
static IOUSBHostInterface *x_intf;
static IOUSBHostPipe *x_in, *x_out;
static unsigned x_gen;   // bumped on every attach and detach: a late callback of an old connection does nothing
static int x_debug;   // DM_XONE_DEBUG=1: log the raw bytes and what was made of them twice a second
static void xone_read(void);
static int x_player = -1;
static uint64_t x_btn;
static int32_t x_lx, x_ly, x_rx, x_ry;
static const char x_owner = 0;   // identity of the pad in the slot table

static void xone_push(void) {
    if (x_player < 0 || !x_input_on || !x_input_on()) return;
    dm_pad_set_player(x_player, x_btn, x_lx, x_ly, x_rx, x_ry);
}
// Focus lost: nothing stays pressed (the front end's input_release). Back in focus: the pad is read as it is now (input_resume).
static void xone_release(void) { if (x_player >= 0) dm_pad_set_player(x_player, 0, 0, 0, 0, 0); }
static void xone_resume(void) { xone_push(); }

// A stick with a round dead zone, scaled to the host's range (the same sign as GameController: up is positive).
static void xone_stick(int16_t rx_, int16_t ry_, int32_t *ox, int32_t *oy) {
    float x = rx_ / 32767.f, y = ry_ / 32767.f, m = hypotf(x, y);
    const float dz = 0.12f;
    if (m < dz) { *ox = *oy = 0; return; }
    float s = fminf(1.f, (m - dz) / (1.f - dz)) / m;
    *ox = (int32_t)(fmaxf(-1.f, fminf(1.f, x * s)) * DM_STICK_MAX);
    *oy = (int32_t)(fmaxf(-1.f, fminf(1.f, y * s)) * DM_STICK_MAX);
}
static void xone_parse(const uint8_t *d, NSUInteger n) {
    if (n < 18 || d[0] != 0x20) return;   // 0x07 is the Guide button, other types are status: ignored
    uint64_t m = 0;
    if (d[4] & 0x04) m |= DM_BTN_PLUS;
    if (d[4] & 0x08) m |= DM_BTN_MINUS;
    if (d[4] & 0x10) m |= DM_BTN_B;   // A, bottom   (by position, as pad_read: right = Switch A, bottom = B, top = X, left = Y)
    if (d[4] & 0x20) m |= DM_BTN_A;   // B, right
    if (d[4] & 0x40) m |= DM_BTN_Y;   // X, left
    if (d[4] & 0x80) m |= DM_BTN_X;   // Y, top
    if (d[5] & 0x01) m |= DM_BTN_UP;
    if (d[5] & 0x02) m |= DM_BTN_DOWN;
    if (d[5] & 0x04) m |= DM_BTN_LEFT;
    if (d[5] & 0x08) m |= DM_BTN_RIGHT;
    if (d[5] & 0x10) m |= DM_BTN_L;
    if (d[5] & 0x20) m |= DM_BTN_R;
    if (d[5] & 0x40) m |= DM_BTN_LS;
    if (d[5] & 0x80) m |= DM_BTN_RS;
    if ((d[6] | d[7] << 8) > 300) m |= DM_BTN_ZL;   // triggers are 0..1023
    if ((d[8] | d[9] << 8) > 300) m |= DM_BTN_ZR;
    x_btn = m;
    xone_stick((int16_t)(d[10] | d[11] << 8), (int16_t)(d[12] | d[13] << 8), &x_lx, &x_ly);
    xone_stick((int16_t)(d[14] | d[15] << 8), (int16_t)(d[16] | d[17] << 8), &x_rx, &x_ry);
    if (x_debug) {
        static uint64_t last;
        uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        if (now - last > 500000000ull) {
            last = now;
            char hex[3 * 24 + 1]; size_t k = n < 24 ? n : 24;
            for (size_t i = 0; i < k; i++) snprintf(hex + 3 * i, 4, "%02x ", d[i]);
            LOG("Xbox One USB: raw [%s] -> buttons 0x%llx, left %d/%d, right %d/%d (raw left %d/%d, right %d/%d, lt %d, rt %d)", hex, (unsigned long long)x_btn, x_lx, x_ly, x_rx, x_ry,
                (int16_t)(d[10] | d[11] << 8), (int16_t)(d[12] | d[13] << 8), (int16_t)(d[14] | d[15] << 8), (int16_t)(d[16] | d[17] << 8), d[6] | d[7] << 8, d[8] | d[9] << 8);
        }
    }
    xone_push();
}

static void xone_close_device(void) {
    if (!x_dev) return;
    IOUSBHostDevice *d = x_dev; x_dev = nil;
    [d destroy];
}
static void xone_detach(void) {
    xone_close_device();
    if (!x_intf) return;
    IOUSBHostInterface *i = x_intf;
    x_gen++;
    x_intf = nil; x_in = nil; x_out = nil;
    [i destroy];
    x_btn = 0; x_lx = x_ly = x_rx = x_ry = 0;
    if (x_player >= 0) {
        dm_pad_set_player(x_player, 0, 0, 0, 0, 0);   // nothing stays pressed
        LOG("controller '%s' disconnected: player %d is free", XONE_NAME, x_player + 1);
        if (!*x_merge) { pad_slot_free(x_slots, &x_owner); dm_pad_players(pad_slot_mask(x_slots)); }
        x_player = -1;
    }
}

static void xone_read_(void) {
    if (!x_intf || !x_in) return;
    NSError *e = nil;
    NSMutableData *buf = [x_intf ioDataWithCapacity:64 error:&e];
    if (!buf) { LOG("Xbox One USB: no I/O buffer: %s", e.localizedDescription.UTF8String); xone_detach(); return; }
    IOUSBHostInterface *mine = x_intf;
    BOOL ok = [x_in enqueueIORequestWithData:buf completionTimeout:0 error:&e completionHandler:^(IOReturn st, NSUInteger n) {
        if (mine != x_intf) return;   // detached meanwhile
        if (st != kIOReturnSuccess) { LOG("Xbox One USB: read ended (0x%x), the pad is released", (unsigned)st); xone_detach(); return; }
        static int shown; if (shown < 8) { shown++; const uint8_t *d = (const uint8_t *)buf.bytes; LOG("Xbox One USB: packet %d: %lu bytes, type 0x%02x", shown, (unsigned long)n, n ? d[0] : 0); }
        xone_parse((const uint8_t *)buf.bytes, n);
        xone_read();
    }];
    if (!ok) { LOG("Xbox One USB: cannot read: %s", e.localizedDescription.UTF8String); xone_detach(); }
}
static void xone_read(void) {
    @try { xone_read_(); }
    @catch (NSException *ex) { LOG("Xbox One USB: exception while reading: %s: %s", ex.name.UTF8String, ex.reason.UTF8String); xone_detach(); }
}

static BOOL xone_send(IOUSBHostPipe *out, NSMutableData *pkt) {
    NSError *e = nil;
    BOOL ok = [out enqueueIORequestWithData:pkt completionTimeout:0 error:&e completionHandler:^(IOReturn st, NSUInteger n) {
        (void)n; if (st != kIOReturnSuccess) LOG("Xbox One USB: the power-on packet failed (0x%x)", (unsigned)st);
    }];
    if (!ok) LOG("Xbox One USB: cannot send the power-on packet: %s", e.localizedDescription.UTF8String);
    return ok;
}

static void xone_attach_(io_service_t svc) {
    NSError *e = nil;
    unsigned gen = ++x_gen;
    IOUSBHostInterface *intf = [[IOUSBHostInterface alloc] initWithIOService:svc options:IOUSBHostObjectInitOptionsNone queue:dispatch_get_main_queue() error:&e
                                                             interestHandler:^(IOUSBHostObject *obj, uint32_t msg, void *arg) {
        (void)obj; (void)arg;
        if (msg == kIOMessageServiceIsTerminated && gen == x_gen) { LOG("Xbox One USB: the pad was unplugged"); xone_detach(); }   // the pad was unplugged
    }];
    if (!intf) { LOG("Xbox One USB: cannot open the interface (%s). Is Steam or another app holding the pad?", e.localizedDescription.UTF8String); return; }
    IOUSBHostPipe *in = [intf copyPipeWithAddress:XONE_EP_IN error:&e];
    IOUSBHostPipe *out = in ? [intf copyPipeWithAddress:XONE_EP_OUT error:&e] : nil;
    if (!in || !out) { LOG("Xbox One USB: endpoints 0x%02x / 0x%02x not found: %s", XONE_EP_IN, XONE_EP_OUT, e.localizedDescription.UTF8String); [intf destroy]; return; }
    int p = 0;
    if (!*x_merge) {
        p = pad_slot_take(x_slots, &x_owner, XONE_NAME);
        if (p < 0) { LOG("controller '%s' connected: all %d players are taken, not used", XONE_NAME, DM_PLAYERS); [intf destroy]; return; }
        dm_pad_players(pad_slot_mask(x_slots));
    }
    x_intf = intf; x_in = in; x_out = out; x_player = p;
    LOG("controller '%s' connected: player %d", XONE_NAME, p + 1);

    // The pad stays dark until it gets this "power on" packet (command 5, payload 0 = on); without it it flashes and gives up after a minute.
    static const uint8_t power_on[5] = { 0x05, 0x20, 0x00, 0x01, 0x00 };
    // First try an I/O buffer of exactly the packet's size (it is never resized: an I/O buffer refuses that), then an ordinary NSMutableData.
    BOOL sent = NO;
    @try {
        NSMutableData *pkt = [intf ioDataWithCapacity:sizeof power_on error:&e];
        if (!pkt) LOG("Xbox One USB: no I/O buffer for the power-on packet: %s", e.localizedDescription.UTF8String);
        else if (pkt.length != sizeof power_on) LOG("Xbox One USB: the I/O buffer is %lu bytes, not %lu", (unsigned long)pkt.length, (unsigned long)sizeof power_on);
        else { memcpy(pkt.mutableBytes, power_on, sizeof power_on); sent = xone_send(out, pkt); }
    }
    @catch (NSException *ex) { LOG("Xbox One USB: exception on the power-on packet (I/O buffer): %s: %s", ex.name.UTF8String, ex.reason.UTF8String); }
    if (!sent) {
        @try { sent = xone_send(out, [NSMutableData dataWithBytes:power_on length:sizeof power_on]); }
        @catch (NSException *ex) { LOG("Xbox One USB: exception on the power-on packet (plain buffer): %s: %s", ex.name.UTF8String, ex.reason.UTF8String); }
    }
    LOG("Xbox One USB: the power-on packet was %s", sent ? "queued" : "NOT sent");
    xone_read();
}

static void xone_attach(io_service_t svc) {
    @try { xone_attach_(svc); }
    @catch (NSException *ex) { LOG("Xbox One USB: exception while attaching: %s: %s", ex.name.UTF8String, ex.reason.UTF8String); xone_detach(); }
}

static void xone_dict_int(CFMutableDictionaryRef d, CFStringRef key, int v) {
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v); CFDictionarySetValue(d, key, n); CFRelease(n);
}

static int xone_prop(io_service_t svc, CFStringRef key) {   // a number property of a registry entry, or -1
    int r = -1;
    CFTypeRef v = IORegistryEntryCreateCFProperty(svc, key, kCFAllocatorDefault, 0);
    if (v) { if (CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &r); CFRelease(v); }
    return r;
}

// The interfaces appear a moment after the configuration is selected. Matching on idVendor / idProduct / bInterfaceNumber in the query found nothing for interfaces
// (it does for the device), so list every IOUSBHostInterface and pick interface 0 of the pad by reading its properties.
static void xone_find_interface(void) {
    int total = 0;
    for (int tries = 0; tries < 40 && !x_intf; tries++) {
        int found = 0; total = 0;
        io_iterator_t it = IO_OBJECT_NULL;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostInterface"), &it) == KERN_SUCCESS) {   // consumes the dictionary
            io_service_t svc;
            while ((svc = IOIteratorNext(it))) {
                total++;
                if (xone_prop(svc, CFSTR("idVendor")) == XONE_VID && xone_prop(svc, CFSTR("idProduct")) == XONE_PID && xone_prop(svc, CFSTR("bInterfaceNumber")) == 0) {
                    found = 1; if (!x_intf) xone_attach(svc);
                }
                IOObjectRelease(svc);
            }
            IOObjectRelease(it);
        }
        if (found) return;   // attached, or failed with its own log line: do not retry forever
        usleep(25000);       // up to 1 s in all
    }
    if (!x_intf) LOG("Xbox One USB: interface 0 of the pad not found among %d USB interfaces", total);
}

// macOS does not configure a device of vendor class (the pad's bDeviceClass is 0xFF): ioreg shows the device with no interface under it, which is why the interface
// notification alone never fired. The device has to be opened and a configuration selected; its interfaces are then registered and xone_find_interface picks interface 0.
static void xone_dev_matched(void *refcon, io_iterator_t it) {
    (void)refcon;
    io_service_t svc;
    while ((svc = IOIteratorNext(it))) {
        if (!x_dev) {
            NSError *e = nil;
            IOUSBHostDevice *dev = [[IOUSBHostDevice alloc] initWithIOService:svc options:IOUSBHostObjectInitOptionsNone queue:dispatch_get_main_queue() error:&e
                                                              interestHandler:^(IOUSBHostObject *obj, uint32_t msg, void *arg) {
                (void)arg;
                if (msg == kIOMessageServiceIsTerminated && obj == (IOUSBHostObject *)x_dev) { LOG("Xbox One USB: the pad was unplugged"); xone_detach(); xone_close_device(); }
            }];
            if (!dev) LOG("Xbox One USB: cannot open the device (%s). Is Steam or another app holding the pad?", e.localizedDescription.UTF8String);
            else {
                x_dev = dev;
                if (dev.configurationDescriptor) { LOG("Xbox One USB: the pad is already configured"); xone_find_interface(); }
                else if ([dev configureWithValue:1 matchInterfaces:YES error:&e]) { LOG("Xbox One USB: the pad is configured, looking for its interface"); xone_find_interface(); }
                else { LOG("Xbox One USB: cannot configure the pad: %s", e.localizedDescription.UTF8String); xone_close_device(); }
            }
        }
        IOObjectRelease(svc);
    }
}

// Called once from pads_start(): watches for the pad (already plugged in, or plugged in later) and takes it over.
static void xone_start(pad_slots *slots, const int *merge, int (*input_on)(void)) {
    x_slots = slots; x_merge = merge; x_input_on = input_on;
    const char *env = getenv("DM_XONE_USB"); if (env && !atoi(env)) return;
    const char *dbg = getenv("DM_XONE_DEBUG"); x_debug = dbg && atoi(dbg);
    x_port = IONotificationPortCreate(kIOMainPortDefault);
    if (!x_port) { LOG("Xbox One USB: no notification port"); return; }
    IONotificationPortSetDispatchQueue(x_port, dispatch_get_main_queue());
    LOG("Xbox One USB: watching for the pad (%04x:%04x)", XONE_VID, XONE_PID);

    CFMutableDictionaryRef d = IOServiceMatching("IOUSBHostDevice");
    xone_dict_int(d, CFSTR("idVendor"), XONE_VID);
    xone_dict_int(d, CFSTR("idProduct"), XONE_PID);
    kern_return_t kr = IOServiceAddMatchingNotification(x_port, kIOFirstMatchNotification, d, xone_dev_matched, NULL, &x_dev_iter);   // consumes d
    if (kr != KERN_SUCCESS) { LOG("Xbox One USB: cannot watch for the device (0x%x)", (unsigned)kr); return; }
    xone_dev_matched(NULL, x_dev_iter);   // a pad that is already plugged in; also arms the notification
}

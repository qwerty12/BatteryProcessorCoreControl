//
//  main.cpp
//  BatteryProcessorCoreControl
//
//  Created by Faheem Pervez on 23/07/2013.
//  Copyright (c) 2013 Faheem Pervez. All rights reserved.
//

/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * Code taken from:
 * https://github.com/RehabMan/OS-X-Voodoo-PS2-Controller/blob/master/VoodooPS2Daemon/main.cpp
 * http://www.virtualbox.org/svn/vbox/trunk/src/VBox/Main/src-server/darwin/HostPowerDarwin.cpp
 * Amit Singh's Mac OS X Internals
 */

#include <cstdio>
#include <cstdlib>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/IOMessage.h>
#include <mach/mach.h>

#define EXIT_ON_MACH_ERROR(msg, retval) \
if (kr != KERN_SUCCESS) { mach_error(msg, kr); exit((retval)); }

static bool g_disableHt = false;
static int g_coresToKeepOn = 2;

static bool g_prevBatteryState = false;
static processor_port_array_t g_processorList;
static mach_msg_type_number_t g_processorCount;
static IONotificationPortRef g_notifyPort;
static io_connect_t g_rootPort;
static io_object_t g_notifierObject;
static bool g_force = false;

static void changeCoresState(bool disable)
{
    mach_msg_type_number_t i = g_processorCount;
    
    while (i-- > g_coresToKeepOn) {
        if (disable)
            processor_exit(g_processorList[i]);
        else
            processor_start(g_processorList[i]);
    }
}

static void powerStateWatcher(__unused void *param_not_used)
{
    bool usingBattery = false;

    CFTypeRef source = IOPSCopyPowerSourcesInfo();
    CFArrayRef powerSources = IOPSCopyPowerSourcesList(source);

    CFIndex count;
    if ((count = CFArrayGetCount(powerSources)) > 0) {
        for (int i = 0; i < count; ++i) {
            const void *psValue;
            CFDictionaryRef powerSource = IOPSGetPowerSourceDescription(source, CFArrayGetValueAtIndex(powerSources, i));
            if (!powerSource)
                continue;
            if (CFDictionaryGetValue(powerSource, CFSTR(kIOPSIsPresentKey)) == kCFBooleanFalse)
                continue;
            if (CFDictionaryGetValueIfPresent(powerSource, CFSTR(kIOPSTransportTypeKey), &psValue) &&
                CFStringCompare((CFStringRef)psValue, CFSTR(kIOPSInternalType), 0) == kCFCompareEqualTo) {
                if (CFDictionaryGetValueIfPresent(powerSource, CFSTR(kIOPSPowerSourceStateKey), &psValue))
                    usingBattery = CFStringCompare((CFStringRef)psValue, CFSTR(kIOPSBatteryPowerValue), 0) == kCFCompareEqualTo;
            }
        }
    }

    if (g_force || usingBattery != g_prevBatteryState) {
        g_force = false;
        changeCoresState(usingBattery);
        g_prevBatteryState = usingBattery;
    }
    
    CFRelease(powerSources);
    CFRelease(source);
}

static void initProcessorControl(int userCoresToKeepOn, mach_port_t host)
{
    kern_return_t          kr;
    host_basic_info_data_t hostInfoData;
    mach_msg_type_number_t hostCount;
    host_priv_t            hostPriv;
    
    hostCount = HOST_BASIC_INFO_COUNT;
    kr = host_info(host, HOST_BASIC_INFO, (host_info_t) &hostInfoData, &hostCount);
    EXIT_ON_MACH_ERROR("host_info:", kr);
    
    kr = host_get_host_priv_port(host, &hostPriv);
    EXIT_ON_MACH_ERROR("host_get_host_priv_port:", kr);
    
    kr = host_processors(hostPriv, &g_processorList, &g_processorCount);
    EXIT_ON_MACH_ERROR("host_processors:", kr);
    
    if (hostInfoData.physical_cpu_max == 1) {
        fprintf(stderr, "You cannot run this on a system with only one processor!\n");
        exit (EXIT_FAILURE);
    }
    else if (hostInfoData.physical_cpu_max == 2) {
        g_coresToKeepOn = 1; //Will always be one on computers with only two cores (although I'm not sure why you'd run this on such a computer)
    }
    else {
        if (g_disableHt && hostInfoData.physical_cpu_max * 2 != hostInfoData.logical_cpu_max) {
            printf("Overriding disable HT setting and setting to false, your system doesn't appear to have HT\n");
            g_disableHt = false;
        }

        if (userCoresToKeepOn > 0 && userCoresToKeepOn < hostInfoData.logical_cpu_max)
            g_coresToKeepOn = userCoresToKeepOn;

        if (!g_disableHt)
            g_coresToKeepOn *= 2;
    }
}

static void powerChangeNotificationHandler(__unused void *param_not_used, __unused io_service_t, natural_t messageType, void *messageArgument)
{
    if (messageType == kIOMessageSystemHasPoweredOn) {
        g_force = true;
        powerStateWatcher(0);
    }

    IOAllowPowerChange(g_rootPort, (long) messageArgument);
}


static void initPowerStateMonitoring(mach_port_t host)
{
    g_rootPort = IORegisterForSystemPower(0, &g_notifyPort,
                                          powerChangeNotificationHandler,
                                          &g_notifierObject);
    if (g_rootPort == MACH_PORT_NULL) {
        fprintf(stderr, "%s: IORegisterForSystemPower returned NULL\n", __PRETTY_FUNCTION__);
        exit (EXIT_FAILURE);
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(g_notifyPort), kCFRunLoopDefaultMode);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), IOPSNotificationCreateRunLoopSource(powerStateWatcher, 0), kCFRunLoopDefaultMode);
    powerStateWatcher(0);
}

static void sighandler(__unused int param_not_used)
{
    CFRunLoopStop(CFRunLoopGetCurrent());
}

static void cleanup()
{
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(g_notifyPort), kCFRunLoopDefaultMode);
    IODeregisterForSystemPower(&g_notifierObject);
    IOServiceClose(g_rootPort);
    IONotificationPortDestroy(g_notifyPort);
    changeCoresState(false);
    (void) vm_deallocate(mach_task_self(), (vm_address_t) g_processorList, g_processorCount * sizeof(processor_t *));
}

int main (int argc, char *argv[])
{
    mach_port_t host = mach_host_self();
    
    int userCoresToKeepOn = 0;
    if (argc > 1)
        userCoresToKeepOn = (int) strtol(argv[1], 0, 10);
    if (argc > 2) {
        if (!strcmp(argv[2], "--ht-off"))
            g_disableHt = true;
    }

    initProcessorControl(userCoresToKeepOn, host);
    initPowerStateMonitoring(host);
    
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    
    CFRunLoopRun();
    
    cleanup();
    
    return EXIT_SUCCESS;
}
/***************************************************************
 *
 * OpenBeacon.org - OpenBeacon link layer protocol
 *
 * Copyright 2007 Milosch Meriac <meriac@openbeacon.de>
 *
 ***************************************************************

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; version 2.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

*/
#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>
#include <string.h>
#include <board.h>
#include <beacontypes.h>
#include <USB-CDC.h>
#include "led.h"
#include "xxtea.h"
#include "debug_printf.h"
#include "proto.h"
#include "nRF24L01/nRF_HW.h"
#include "nRF24L01/nRF_CMD.h"
#include "nRF24L01/nRF_API.h"
#include "detector-grid.h"

// maximum number of tags the system can handle at the same time
#define BEACON_STACK_SIZE 8
// number of RTOS ticks before a tag is removed from the stack
#define BEACON_CUTOFF_TICKS 8096

const unsigned char broadcast_mac[NRF_MAX_MAC_SIZE] = { 1, 2, 3, 2, 1 };

TBeaconEnvelope g_Beacon;

// global beacon stack for storing the last seen beacons
TBeaconInfo g_BeaconStack[BEACON_STACK_SIZE];

/**
 * Add a beacon to the internal stack, taking into account cutoff times & minimum power ratings
 * @param beacon Beacon to add
 * @return Reference to entry in global beacon stack, or NULL if stack is full
 */
TBeaconInfo *AddBeaconToStack(TBeaconInfo *beacon) {
    int i = 0;
    portTickType currTickCount = xTaskGetTickCount();
    
    // search the whole beacon stack for old entry
    for( i = 0; i < BEACON_STACK_SIZE; i++ ) {
        // check for a beacon match
        if( g_BeaconStack[i].oid == beacon->oid ) {
            // now check if strength is more accurate or the entry is more recent then the tick cutoff
            if( g_BeaconStack[i].strength >= beacon->strength || g_BeaconStack[i].seenTick < (currTickCount - BEACON_CUTOFF_TICKS) ) {
                g_BeaconStack[i].seenTick = beacon->seenTick;
                g_BeaconStack[i].strength = beacon->strength;
                
            }
            
            return &g_BeaconStack[i];
        }
    }
    
    // if no old entry is found, add a new one
    for( i = 0; i < BEACON_STACK_SIZE; i++ ) {
        // check for outdated entry
        if( g_BeaconStack[i].seenTick < (currTickCount - BEACON_CUTOFF_TICKS) ) {
            g_BeaconStack[i].oid = beacon->oid;
            g_BeaconStack[i].seenTick = beacon->seenTick;
            g_BeaconStack[i].strength = beacon->strength;
            
            return &g_BeaconStack[i];
        }
    }
    
    // if we reach here, the stack is fully used
    return NULL;
}

/**********************************************************************/
#define SHUFFLE(a,b)    tmp=g_Beacon.byte[a];\
                        g_Beacon.byte[a]=g_Beacon.byte[b];\
                        g_Beacon.byte[b]=tmp;

/**********************************************************************/
void
shuffle_tx_byteorder (void)
{
	unsigned char tmp;

	SHUFFLE (0 + 0, 3 + 0);
	SHUFFLE (1 + 0, 2 + 0);
	SHUFFLE (0 + 4, 3 + 4);
	SHUFFLE (1 + 4, 2 + 4);
	SHUFFLE (0 + 8, 3 + 8);
	SHUFFLE (1 + 8, 2 + 8);
	SHUFFLE (0 + 12, 3 + 12);
	SHUFFLE (1 + 12, 2 + 12);
}

static inline s_int8_t
PtInitNRF (void)
{
	if (!nRFAPI_Init
		(DEFAULT_DEV, DEFAULT_CHANNEL, broadcast_mac, sizeof (broadcast_mac),
		 0))
		return 0;

	nRFAPI_SetPipeSizeRX (DEFAULT_DEV, 0, 16);
	nRFAPI_SetTxPower (DEFAULT_DEV, 3);
	nRFAPI_SetRxMode (DEFAULT_DEV, 1);
	nRFCMD_CE (DEFAULT_DEV, 1);

	return 1;
}

static inline unsigned short
swapshort (unsigned short src)
{
	return (src >> 8) | (src << 8);
}

static inline unsigned long
swaplong (unsigned long src)
{
	return (src >> 24) | (src << 24) | ((src >> 8) & 0x0000FF00) | ((src << 8)
																	&
																	0x00FF0000);
}

static inline short
crc16 (const unsigned char *buffer, int size)
{
	unsigned short crc = 0xFFFF;

	if (buffer && size)
		while (size--)
		{
			crc = (crc >> 8) | (crc << 8);
			crc ^= *buffer++;
			crc ^= ((unsigned char) crc) >> 4;
			crc ^= crc << 12;
			crc ^= (crc & 0xFF) << 5;
		}

	return crc;
}

static inline void
DumpUIntToUSB (unsigned int data)
{
	int i = 0;
	unsigned char buffer[10], *p = &buffer[sizeof (buffer)];

	do
	{
		*--p = '0' + (unsigned char) (data % 10);
		data /= 10;
		i++;
	}
	while (data);

	while (i--)
		vUSBSendByte (*p++);
}

static inline void
DumpStringToUSB (char *text)
{
	unsigned char data;

	if (text)
		while ((data = *text++) != 0)
			vUSBSendByte (data);
}

void
vnRFtaskRx (void *parameter)
{
	u_int16_t crc;
	(void) parameter;
        TBeaconInfo beaconInfo;

	if (!PtInitNRF ())
		return;

	vLedSetGreen (1);

	for (;;)
	{
		if (nRFCMD_WaitRx (10))
		{
			vLedSetRed (1);

			do
			{
				// read packet from nRF chip
				nRFCMD_RegReadBuf (DEFAULT_DEV, RD_RX_PLOAD, g_Beacon.byte,
								   sizeof (g_Beacon));

				// adjust byte order and decode
				shuffle_tx_byteorder ();
				xxtea_decode ();
				shuffle_tx_byteorder ();

				// verify the crc checksum
				crc =
					crc16 (g_Beacon.byte,
						   sizeof (g_Beacon) - sizeof (g_Beacon.pkt.crc));
				if ((swapshort (g_Beacon.pkt.crc) == crc)) {
                                    // check which protocol is used for this package
                                    switch( g_Beacon.pkt.proto ) {
                                        // proxreport uses strength 3
                                        case RFBPROTO_PROXREPORT:
                                        case RFBPROTO_PROXREPORT_EXT:
                                            g_Beacon.pkt.p.tracker.strength = 3;
                                            break;
                                        // default protocol
                                        case RFBPROTO_BEACONTRACKER:
                                            break;
                                    }
                                    
                                    // show debug info
                                    debug_printf("Proto used: %d\n", g_Beacon.pkt.proto);
                                    debug_printf("TX Power in packet: %d\n", g_Beacon.pkt.p.tracker.strength);
                                    
                                    // setup beacon info
                                    beaconInfo.oid = swapshort(g_Beacon.pkt.oid);
                                    beaconInfo.strength = g_Beacon.pkt.p.tracker.strength;
                                    beaconInfo.seenTick = xTaskGetTickCount();
                                    
                                    // add beaconinfo to global stack
                                    TBeaconInfo *currBeaconInfo = AddBeaconToStack(&beaconInfo);
                                    if( currBeaconInfo == NULL ) {
                                        debug_printf("ERROR: Stack is full!\n");
                                    }
                                    else {
                                        debug_printf("TAG: %04i,%i,%i\n", currBeaconInfo->oid, currBeaconInfo->strength, currBeaconInfo->seenTick);
                                    }
                                }
			}
			while ((nRFAPI_GetFifoStatus (DEFAULT_DEV) & FIFO_RX_EMPTY) == 0);

			vLedSetRed (0);
			nRFAPI_GetFifoStatus (DEFAULT_DEV);
		}
		nRFAPI_ClearIRQ (DEFAULT_DEV, MASK_IRQ_FLAGS);
	}
}

void
vInitProtocolLayer (void)
{
	xTaskCreate (vnRFtaskRx, (signed portCHAR *) "nRF_Rx", TASK_NRF_STACK,
				 NULL, TASK_NRF_PRIORITY, NULL);
}
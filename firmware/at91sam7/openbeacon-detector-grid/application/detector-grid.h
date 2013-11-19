/* 
 * File:   detector-grid.h
 * Author: wkoller
 *
 * Created on 19. November 2013, 17:49
 */

#ifndef DETECTOR_GRID_H
#define	DETECTOR_GRID_H

#include "openbeacon.h"

typedef struct {
    u_int16_t oid;
    u_int8_t strength;
    portTickType seenTick;
} PACKED TBeaconInfo;

#endif	/* DETECTOR_GRID_H */


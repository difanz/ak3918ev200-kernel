#ifndef ECHOS_PRO_TABLES_H
#define ECHOS_PRO_TABLES_H

#include <linux/types.h>

/*
 * smoothWindow[] and comfortNoise[] are private lookup tables consumed only
 * from within echos_pro_tables.c; they carry no external declaration here
 * (kernel-build staticisation, see echos_pro_tables.c). The length macros
 * are kept for documentation/reference.
 */
#define SMOOTH_WINDOW_LEN 129
#define COMFORT_NOISE_LEN 2307

#endif

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * GPS (WGS-84) -> AMap / GCJ-02. Inside China the two differ by 300-500 m, so a raw GPS
 * fix drawn on an AMap map (or matched to an AMap route) lands in the wrong street.
 * Outside China the input is returned unchanged.
 */
void wgs84_to_gcj02(double lat, double lon, double *out_lat, double *out_lon);

#ifdef __cplusplus
}
#endif

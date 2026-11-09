#include "coord_cn.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static bool out_of_china(double lat, double lon)
{
    return lon < 72.004 || lon > 137.8347 || lat < 0.8293 || lat > 55.8271;
}

static double transform_lat(double x, double y)
{
    double r = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * sqrt(fabs(x));
    r += (20.0 * sin(6.0 * x * M_PI) + 20.0 * sin(2.0 * x * M_PI)) * 2.0 / 3.0;
    r += (20.0 * sin(y * M_PI) + 40.0 * sin(y / 3.0 * M_PI)) * 2.0 / 3.0;
    r += (160.0 * sin(y / 12.0 * M_PI) + 320.0 * sin(y * M_PI / 30.0)) * 2.0 / 3.0;
    return r;
}

static double transform_lon(double x, double y)
{
    double r = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * sqrt(fabs(x));
    r += (20.0 * sin(6.0 * x * M_PI) + 20.0 * sin(2.0 * x * M_PI)) * 2.0 / 3.0;
    r += (20.0 * sin(x * M_PI) + 40.0 * sin(x / 3.0 * M_PI)) * 2.0 / 3.0;
    r += (150.0 * sin(x / 12.0 * M_PI) + 300.0 * sin(x / 30.0 * M_PI)) * 2.0 / 3.0;
    return r;
}

void wgs84_to_gcj02(double lat, double lon, double *out_lat, double *out_lon)
{
    if (out_of_china(lat, lon)) {
        *out_lat = lat;
        *out_lon = lon;
        return;
    }
    const double a  = 6378245.0;
    const double ee = 0.00669342162296594323;

    double dlat = transform_lat(lon - 105.0, lat - 35.0);
    double dlon = transform_lon(lon - 105.0, lat - 35.0);
    double radlat = lat / 180.0 * M_PI;
    double magic = sin(radlat);
    magic = 1.0 - ee * magic * magic;
    double sqrtmagic = sqrt(magic);
    dlat = (dlat * 180.0) / ((a * (1.0 - ee)) / (magic * sqrtmagic) * M_PI);
    dlon = (dlon * 180.0) / (a / sqrtmagic * cos(radlat) * M_PI);

    *out_lat = lat + dlat;
    *out_lon = lon + dlon;
}

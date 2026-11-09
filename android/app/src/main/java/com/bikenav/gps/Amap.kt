package com.bikenav.gps

import org.json.JSONArray
import org.json.JSONObject
import java.net.URLEncoder

class AmapException(message: String) : Exception(message)

/**
 * AMap REST ("Web服务") client, used only to re-plan a route while the phone screen is off (the web page
 * does the normal planning with the JavaScript API). Needs a key of type "Web服务"; a JavaScript-API key
 * is rejected with USERKEY_PLAT_NOMATCH.
 */
class Amap(private val key: String) {
    companion object {
        private const val BASE = "https://restapi.amap.com"

        private fun str(o: JSONObject, k: String): String {
            val v = o.opt(k)
            return when (v) {                         // AMap sends [] for an empty text field
                is String -> v
                is JSONArray -> if (v.length() > 0) v.optString(0, "") else ""
                else -> ""
            }
        }

        fun errorText(code: String?, msg: String?): String = when (code) {
            "10001", "INVALID_USER_KEY" -> "AMap says the key is invalid. Check it in the app settings."
            "10009", "USERKEY_PLAT_NOMATCH" ->
                "This AMap key is not a 'Web服务' (web service) key. Create one at console.amap.com."
            "10003", "10004", "10010", "10011", "DAILY_QUERY_OVER_LIMIT", "ACCESS_TOO_FREQUENT" ->
                "AMap quota or rate limit reached. Try again later."
            else -> "AMap error: ${msg ?: code ?: "unknown"}"
        }

        private fun lngLat(s: String): LngLat? {
            val p = s.split(",")
            if (p.size < 2) return null
            val lng = p[0].trim().toDoubleOrNull() ?: return null
            val lat = p[1].trim().toDoubleOrNull() ?: return null
            return LngLat(lng, lat)
        }

        /** Understands both the v4 and the v5 bicycling response. */
        fun parseBicycling(root: JSONObject, destName: String, destPos: LngLat): Route {
            val paths: JSONArray = when {
                root.has("data") && !root.isNull("data") -> {                          // v4
                    root.getJSONObject("data").optJSONArray("paths")
                }
                root.has("route") && !root.isNull("route") -> {                        // v5
                    root.getJSONObject("route").optJSONArray("paths")
                }
                else -> {
                    val code = if (root.has("errcode")) root.optString("errcode") else root.optString("infocode")
                    val msg = if (root.has("errmsg")) root.optString("errmsg") else root.optString("info")
                    throw AmapException(errorText(code, msg))
                }
            } ?: throw AmapException("No route found")
            if (paths.length() == 0) throw AmapException("No route found")

            val path = paths.getJSONObject(0)
            val pts = ArrayList<LngLat>()
            val steps = ArrayList<Step>()
            var stepSeconds = 0.0
            val rawSteps = path.optJSONArray("steps") ?: throw AmapException("The route has no steps")
            for (i in 0 until rawSteps.length()) {
                val s = rawSteps.getJSONObject(i)
                val poly = str(s, "polyline")
                if (poly.isEmpty()) continue
                val road = if (s.has("road")) str(s, "road") else str(s, "road_name")
                var first = true
                for (pair in poly.split(";")) {
                    val p = lngLat(pair) ?: continue
                    if (pts.isNotEmpty() && pts.last() == p) {                   // the junction point repeats
                        if (first) { steps.add(Step(pts.size - 1, road)); first = false }
                        continue
                    }
                    if (first) { steps.add(Step(pts.size, road)); first = false }
                    pts.add(p)
                }
                stepSeconds += s.optDouble("duration", s.optJSONObject("cost")?.optDouble("duration", 0.0) ?: 0.0)
            }
            if (pts.size < 2) throw AmapException("The route has no geometry")

            val dist = path.optDouble("distance", Double.NaN)
            var length = 0.0
            for (i in 1 until pts.size) length += Geo.distanceM(pts[i - 1], pts[i])
            val distance = if (dist.isNaN()) length else dist
            var duration = path.optDouble("duration", Double.NaN)
            if (duration.isNaN()) duration = if (stepSeconds > 0) stepSeconds else distance / 4.2   // ~15 km/h
            return Route(pts, steps, distance, duration, destName, destPos)
        }
    }

    private fun enc(s: String) = URLEncoder.encode(s, "UTF-8")

    fun bicycling(from: LngLat, to: LngLat, destName: String): Route {
        val o = "${from.lng},${from.lat}"
        val d = "${to.lng},${to.lat}"
        val v4 = try {
            parseBicycling(JSONObject(Http.get("$BASE/v4/direction/bicycling?key=$key&origin=$o&destination=$d")), destName, to)
        } catch (e: AmapException) {
            if (e.message?.contains("key", ignoreCase = true) == true) throw e     // a bad key fails on v5 too
            null
        }
        if (v4 != null) return v4
        return parseBicycling(
            JSONObject(Http.get("$BASE/v5/direction/bicycling?key=$key&origin=$o&destination=$d&show_fields=polyline,cost")),
            destName, to)
    }
}

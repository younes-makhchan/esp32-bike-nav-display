package com.bikenav.gps

import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.abs
import kotlin.math.roundToInt

/** A planned ride. [steps] mark where the road changes: index into [points] and the road entered there. */
class Step(val idx: Int, val road: String)

class Maneuver(val idx: Int, val angle: Double, val road: String)

class Route(
    val points: List<LngLat>,
    val steps: List<Step>,
    val distanceM: Double,
    val durationS: Double,
    val destName: String,
    val destPos: LngLat
) {
    companion object {
        const val MIN_TURN_DEG = 25.0           // smaller changes are just a road-name change
        const val LIGHT_MIN_SPACING_M = 90.0
        const val LIGHT_CLEAR_ENDS_M = 60.0
    }

    /** Metres from the start to each point. */
    val cum: DoubleArray by lazy {
        val c = DoubleArray(points.size)
        for (i in 1 until points.size) c[i] = c[i - 1] + Geo.distanceM(points[i - 1], points[i])
        c
    }

    /** Route direction ~12 m before (-1) or after (+1) a vertex. */
    private fun headingAt(idx: Int, dir: Int): Double? {
        val p = points[idx]
        var j = idx + dir
        while (j >= 0 && j < points.size) {
            if (Geo.distanceM(p, points[j]) >= 12.0) {
                return if (dir > 0) Geo.bearingDeg(p, points[j]) else Geo.bearingDeg(points[j], p)
            }
            j += dir
        }
        return null
    }

    /** The real turns: where the direction after a step start differs enough from the direction before it. */
    val maneuvers: List<Maneuver> by lazy {
        val list = ArrayList<Maneuver>()
        for (k in 1 until steps.size) {
            val idx = steps[k].idx
            if (idx <= 0 || idx >= points.size) continue
            val a = headingAt(idx, -1) ?: continue
            val b = headingAt(idx, 1) ?: continue
            val d = Geo.norm180(b - a)                    // + = right, - = left
            if (abs(d) < MIN_TURN_DEG) continue
            list.add(Maneuver(idx, d, steps[k].road))
        }
        list
    }

    /** Traffic-light badges: AMap gives no signal data, so they go where the route changes road. */
    fun trafficLights(): List<LngLat> {
        val out = ArrayList<LngLat>()
        var last: LngLat? = null
        for (k in 1 until steps.size) {
            val idx = steps[k].idx
            if (idx >= points.size) continue
            val p = points[idx]
            if (Geo.distanceM(p, points.first()) < LIGHT_CLEAR_ENDS_M || Geo.distanceM(p, destPos) < LIGHT_CLEAR_ENDS_M) continue
            if (last != null && Geo.distanceM(p, last) < LIGHT_MIN_SPACING_M) continue
            out.add(p)
            last = p
        }
        return out
    }

    private fun r6(v: Double) = Math.round(v * 1e6) / 1e6

    /** Body of POST /api/route on the bike unit. */
    fun toDeviceJson(): String {
        val turnAt = HashMap<Int, Int>()
        for (m in maneuvers) turnAt[m.idx] = m.angle.roundToInt()

        val wp = JSONArray()
        for (p in Geo.simplify(points, 1.5, 700)) wp.put(JSONArray().put(r6(p.lng)).put(r6(p.lat)))
        val tl = JSONArray()
        for (p in trafficLights()) tl.put(JSONArray().put(r6(p.lng)).put(r6(p.lat)))
        val st = JSONArray()
        for (s in steps) {
            if (s.idx >= points.size) continue
            val p = points[s.idx]
            st.put(JSONArray().put(r6(p.lng)).put(r6(p.lat)).put(turnAt[s.idx] ?: 0).put(s.road))
        }
        return JSONObject()
            .put("dest_name", destName)
            .put("dest_lat", destPos.lat)
            .put("dest_lon", destPos.lng)
            .put("waypoints", wp)
            .put("traffic_lights", tl)
            .put("st", st)
            .toString()
    }
}

/** The route as the web page in the app hands it over: path [[lng,lat],..], steps [{idx,road}], dest [lng,lat]. */
fun routeFromPageJson(o: JSONObject): Route {
    val path = o.getJSONArray("path")
    val pts = ArrayList<LngLat>(path.length())
    for (i in 0 until path.length()) {
        val p = path.getJSONArray(i)
        pts.add(LngLat(p.getDouble(0), p.getDouble(1)))
    }
    require(pts.size >= 2) { "route has no geometry" }
    val raw = o.optJSONArray("steps") ?: JSONArray()
    val steps = ArrayList<Step>(raw.length())
    for (i in 0 until raw.length()) {
        val s = raw.getJSONObject(i)
        steps.add(Step(s.getInt("idx"), s.optString("road", "")))
    }
    if (steps.isEmpty()) steps.add(Step(0, ""))
    val d = o.getJSONArray("dest")
    return Route(pts, steps, o.optDouble("distance", 0.0), o.optDouble("time", 0.0),
        o.optString("destName", "目的地"), LngLat(d.getDouble(0), d.getDouble(1)))
}

/** Follows a rider along a route: progress, off-route detection (3 fixes in a row), arrival. */
class RouteMatcher(val route: Route) {
    companion object {
        const val OFF_ROUTE_M = 60.0
        const val ARRIVE_M = 25.0
        const val WINDOW_M = 400.0
        const val OFF_COUNT = 3
    }

    class Result(val crossM: Double, val alongM: Double, val offRouteConfirmed: Boolean, val arrived: Boolean)

    private var seg = 0
    private var off = 0

    fun update(p: LngLat): Result {
        val pts = route.points
        val cum = route.cum
        var best = -1
        var bd = Double.MAX_VALUE
        var bt = 0.0
        var blen = 0.0
        var i = seg
        while (i < pts.size - 1) {
            if (cum[i] - cum[seg] > WINDOW_M) break
            val pr = Geo.project(p, pts[i], pts[i + 1])
            if (pr.distM < bd) { bd = pr.distM; best = i; bt = pr.t; blen = pr.lenM }
            i++
        }
        var confirmed = false
        if (best < 0 || bd > OFF_ROUTE_M) {
            off++
            if (off >= OFF_COUNT) { confirmed = true; off = 0 }
        } else {
            off = 0
            seg = best
        }
        val along = if (best >= 0) cum[best] + bt * blen else cum[seg]
        val arrived = Geo.distanceM(p, route.destPos) < ARRIVE_M
        return Result(if (best >= 0) bd else Double.MAX_VALUE, along, confirmed, arrived)
    }
}

/** What is being ridden. Activity and service share it (one process). */
object Ride {
    @Volatile var active: Route? = null       // sent to the unit and being followed
}

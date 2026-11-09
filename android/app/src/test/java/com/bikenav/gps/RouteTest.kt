package com.bikenav.gps

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

class RouteTest {
    // East 193 m along "A路", then north 222 m along "B路": a left turn at the junction.
    private val v4 = """
    {"errcode":0,"errmsg":"OK","errdetail":null,"data":{"origin":"120.0,30.0","destination":"120.002,30.002","paths":[
      {"distance":415,"duration":120,"steps":[
        {"instruction":"沿A路骑行193米","orientation":"东","road":"A路","distance":193,"duration":55,
         "polyline":"120.0000,30.0000;120.0005,30.0000;120.0010,30.0000;120.0015,30.0000;120.0020,30.0000"},
        {"instruction":"左转","orientation":"北","road":"B路","distance":222,"duration":65,
         "polyline":"120.0020,30.0000;120.0020,30.0005;120.0020,30.0010;120.0020,30.0015;120.0020,30.0020"}
      ]}]}}"""

    private fun route() = Amap.parseBicycling(JSONObject(v4), "目的地", LngLat(120.002, 30.002))

    @Test fun parsesGeometryAndSteps() {
        val r = route()
        assertEquals(9, r.points.size)                       // the repeated junction point is not doubled
        assertEquals(2, r.steps.size)
        assertEquals("A路", r.steps[0].road)
        assertEquals(4, r.steps[1].idx)                      // step 2 starts AT the shared junction vertex
        assertEquals("B路", r.steps[1].road)
        assertEquals(415.0, r.distanceM, 0.01)
        assertEquals(120.0, r.durationS, 0.01)
    }

    @Test fun findsTheLeftTurn() {
        val m = route().maneuvers
        assertEquals(1, m.size)
        assertEquals(-90.0, m[0].angle, 1.0)                 // - = left
        assertEquals("B路", m[0].road)
    }

    @Test fun deviceJsonCarriesStepsAndAngles() {
        val o = JSONObject(route().toDeviceJson())
        val st = o.getJSONArray("st")
        assertEquals(2, st.length())
        assertEquals(0, st.getJSONArray(0).getInt(2))
        assertEquals(-90, st.getJSONArray(1).getInt(2))
        assertEquals("B路", st.getJSONArray(1).getString(3))
        assertEquals(120.002, st.getJSONArray(1).getDouble(0), 1e-6)
        assertEquals(30.002, o.getDouble("dest_lat"), 1e-9)
        assertTrue(o.getJSONArray("waypoints").length() in 3..9)      // straight runs collapse, corner kept
        assertEquals("目的地", o.getString("dest_name"))
    }

    @Test fun trafficLightAtTheJunction() {
        val tl = route().trafficLights()
        assertEquals(1, tl.size)
        assertEquals(120.002, tl[0].lng, 1e-6)
    }

    @Test fun parsesV5Shape() {
        val v5 = """{"status":"1","info":"OK","infocode":"10000","route":{"paths":[{"distance":"415","steps":[
          {"instruction":"x","road_name":"A路","step_distance":"193","cost":{"duration":"55"},"polyline":"120.0,30.0;120.002,30.0"},
          {"instruction":"y","road_name":"B路","step_distance":"222","cost":{"duration":"65"},"polyline":"120.002,30.0;120.002,30.002"}]}]}}"""
        val r = Amap.parseBicycling(JSONObject(v5), "x", LngLat(120.002, 30.002))
        assertEquals(3, r.points.size)
        assertEquals("B路", r.steps[1].road)
        assertEquals(415.0, r.distanceM, 0.01)
        assertEquals(120.0, r.durationS, 0.01)               // summed from the steps
    }

    @Test fun errorsAreReadable() {
        try {
            Amap.parseBicycling(JSONObject("""{"errmsg":"USERKEY_PLAT_NOMATCH","errcode":10009}"""), "x", LngLat(0.0, 0.0))
            fail("should throw")
        } catch (e: AmapException) {
            assertTrue(e.message!!.contains("Web服务"))
        }
    }

    @Test fun routeFromThePageJson() {
        val page = """{"path":[[120.0,30.0],[120.001,30.0],[120.002,30.0],[120.002,30.001],[120.002,30.002]],
          "steps":[{"idx":0,"road":"A路"},{"idx":2,"road":"B路"}],"distance":415,"time":120,
          "destName":"终点","dest":[120.002,30.002]}"""
        val r = routeFromPageJson(JSONObject(page))
        assertEquals(5, r.points.size)
        assertEquals("终点", r.destName)
        assertEquals(30.002, r.destPos.lat, 1e-9)
        assertEquals(-90.0, r.maneuvers.single().angle, 1.0)
        val st = JSONObject(r.toDeviceJson()).getJSONArray("st")
        assertEquals(-90, st.getJSONArray(1).getInt(2))
    }

    /** A real route captured from the page (AMap JS Riding, 8.4 km): the exact shape the app receives. */
    @Test fun realRouteFromThePage() {
        val text = javaClass.getResource("/page_route_sample.json")!!.readText()
        val r = routeFromPageJson(JSONObject(text))
        assertEquals(317, r.points.size)
        assertEquals(10, r.steps.size)
        assertTrue(r.maneuvers.isNotEmpty() && r.maneuvers.size < r.steps.size)

        val dev = JSONObject(r.toDeviceJson())
        val wp = dev.getJSONArray("waypoints")
        assertTrue("waypoints=${wp.length()}", wp.length() in 10..700)
        assertEquals(10, dev.getJSONArray("st").length())
        assertTrue("message size", r.toDeviceJson().length < 20000)       // what the board has to accept

        // Riding along it: progress only grows, never off route, ends at the end of the line.
        val m = RouteMatcher(r)
        var last = 0.0
        for (p in r.points) {
            val res = m.update(p)
            assertFalse(res.offRouteConfirmed)
            assertTrue(res.alongM >= last - 1.0)
            last = res.alongM
        }
        assertEquals(r.cum.last(), last, 30.0)
    }

    @Test fun matcherFollowsProgressAndDetectsOffRoute() {
        val r = route()
        val m = RouteMatcher(r)
        val on = m.update(LngLat(120.001, 30.00001))                  // ~96 m along, ~1 m off the line
        assertEquals(96.0, on.alongM, 3.0)
        assertTrue(on.crossM < 3.0)
        assertFalse(on.offRouteConfirmed)
        assertFalse(on.arrived)
        val far = LngLat(120.001, 30.001)                              // ~111 m north of the road
        assertFalse(m.update(far).offRouteConfirmed)
        assertFalse(m.update(far).offRouteConfirmed)
        assertTrue(m.update(far).offRouteConfirmed)                    // third in a row
        assertTrue(m.update(LngLat(120.002, 30.00199)).arrived)
    }

    @Test fun conversionMatchesPublishedExample() {
        val g = Geo.wgs84ToGcj02(31.1774276, 121.5272106)
        assertEquals(31.17530398364597, g.lat, 1e-9)
        assertEquals(121.531541859215, g.lng, 1e-9)
        val abroad = Geo.wgs84ToGcj02(48.85, 2.35)
        assertEquals(2.35, abroad.lng, 0.0)
    }

    @Test fun simplifyKeepsCornersAndDropsStraightRuns() {
        val pts = ArrayList<LngLat>()
        for (i in 0..1000) pts.add(LngLat(120.0 + i * 1e-5, 30.0))
        for (i in 1..1000) { val a = i / 1000.0 * Math.PI / 2; pts.add(LngLat(120.01 + 0.0005 * Math.sin(a), 30.0 + 0.0005 * (1 - Math.cos(a)))) }
        val out = Geo.simplify(pts, 1.5, 700)
        assertEquals(pts.first(), out.first())
        assertEquals(pts.last(), out.last())
        assertTrue(out.size in 4..12)
    }
}

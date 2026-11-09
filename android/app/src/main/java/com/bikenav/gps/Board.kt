package com.bikenav.gps

import java.net.Inet4Address
import java.net.HttpURLConnection
import java.net.NetworkInterface
import java.net.URL
import java.util.concurrent.Callable
import java.util.concurrent.Executors

/** Finding the bike unit on the phone's hotspot. The unit answers GET /api/ping with "bike-nav". */
object Board {
    fun ping(host: String): Boolean = try {
        val c = URL("http://$host/api/ping").openConnection() as HttpURLConnection
        c.connectTimeout = 600
        c.readTimeout = 600
        val ok = c.inputStream.use { s -> String(s.readNBytes(16)) == "bike-nav" }
        c.disconnect()
        ok
    } catch (e: Exception) {
        false
    }

    fun post(host: String, path: String, body: String): Boolean = try {
        Http.post("http://$host$path", body)
    } catch (e: Exception) {
        false
    }

    /** The /24 networks of this phone's interfaces (the hotspot is one of them). */
    private fun subnets(): List<String> {
        val nets = ArrayList<String>()
        try {
            for (ni in NetworkInterface.getNetworkInterfaces()) {
                if (!ni.isUp || ni.isLoopback) continue
                val n = ni.name
                if (n.startsWith("rmnet") || n.startsWith("dummy") || n.startsWith("tun") ||
                    n.startsWith("ccmni") || n.startsWith("ifb")) continue
                for (a in ni.inetAddresses) {
                    if (a is Inet4Address) nets.add(a.hostAddress!!.substringBeforeLast('.'))
                }
            }
        } catch (_: Exception) {
        }
        nets.add("192.168.43")            // the classic Android hotspot subnet
        return nets.distinct()
    }

    /** Try the saved address, then scan the hotspot's subnets. */
    fun find(cached: String?): String? {
        if (!cached.isNullOrEmpty() && ping(cached)) return cached
        val hosts = subnets().flatMap { net -> (2..254).map { "$net.$it" } }
        val pool = Executors.newFixedThreadPool(48)
        try {
            val jobs = hosts.map { h -> pool.submit(Callable { if (ping(h)) h else null }) }
            for (j in jobs) j.get()?.let { return it }
        } finally {
            pool.shutdownNow()
        }
        return null
    }
}

package com.bikenav.gps

import java.net.HttpURLConnection
import java.net.URL

object Http {
    fun get(url: String, timeoutMs: Int = 8000): String = String(getBytes(url, timeoutMs), Charsets.UTF_8)

    fun getBytes(url: String, timeoutMs: Int = 12000): ByteArray {
        val c = URL(url).openConnection() as HttpURLConnection
        c.connectTimeout = timeoutMs
        c.readTimeout = timeoutMs
        try {
            return c.inputStream.use { it.readBytes() }
        } finally {
            c.disconnect()
        }
    }

    /** POST JSON; the reply body on HTTP 200, otherwise null. */
    fun postRead(url: String, body: String, timeoutMs: Int = 1500): String? {
        val c = URL(url).openConnection() as HttpURLConnection
        try {
            c.requestMethod = "POST"
            c.doOutput = true
            c.connectTimeout = timeoutMs
            c.readTimeout = timeoutMs
            c.setRequestProperty("Content-Type", "application/json")
            c.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }
            if (c.responseCode != 200) return null
            return c.inputStream.use { String(it.readBytes(), Charsets.UTF_8) }
        } finally {
            c.disconnect()
        }
    }

    /** POST JSON; true on HTTP 200. */
    fun post(url: String, body: String, timeoutMs: Int = 5000): Boolean {
        val c = URL(url).openConnection() as HttpURLConnection
        try {
            c.requestMethod = "POST"
            c.doOutput = true
            c.connectTimeout = timeoutMs
            c.readTimeout = timeoutMs
            c.setRequestProperty("Content-Type", "application/json")
            c.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }
            return c.responseCode == 200
        } finally {
            c.disconnect()
        }
    }
}
